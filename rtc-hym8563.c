/* drivers/rtc/rtc-HYM8563.c - driver for HYM8563
 *
 * Copyright (C) 2010 ROCKCHIP, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

//#define DEBUG
#define pr_fmt(fmt) "rtc: %s: " fmt, __func__

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/bcd.h>
#include <linux/rtc.h>
#include <linux/delay.h>
#include <linux/wakelock.h>
#include <linux/slab.h>
#include <linux/miscdevice.h>
#include <linux/clk-provider.h>
#include <linux/time.h>
#include "rtc-HYM8563.h"
#include <linux/of_gpio.h>
#include <linux/irqdomain.h>
#define RTC_SPEED 	200 * 1000

#define    XHRTC_SET_ALARM             0x1f2
#define    XHRTC_CANALE_ALARM          0x1f3

#define HYM8563_CLKOUT		0x0d
#define HYM8563_CLKOUT_ENABLE	BIT(7)
#define HYM8563_CLKOUT_32768	0
#define HYM8563_CLKOUT_1024	1
#define HYM8563_CLKOUT_32	2
#define HYM8563_CLKOUT_1	3
#define HYM8563_CLKOUT_MASK	3

struct hym8563 {
	int irq;
	struct i2c_client *client;
	struct mutex mutex;
	struct rtc_device *rtc;
	struct rtc_wkalrm alarm;
	struct wake_lock wake_lock;
	
	#ifdef CONFIG_COMMON_CLK
	struct clk_hw		clkout_hw;
	#endif
};
static struct i2c_client *gClient = NULL;
static struct hym8563 *g_hym8563 = NULL;

static int i2c_master_reg8_send(const struct i2c_client *client, const char reg, const char *buf, int count, int scl_rate)
{
	struct i2c_adapter *adap=client->adapter;
	struct i2c_msg msg;
	int ret;
	char *tx_buf = (char *)kzalloc(count + 1, GFP_KERNEL);
	if(!tx_buf)
		return -ENOMEM;
	tx_buf[0] = reg;
	memcpy(tx_buf+1, buf, count); 

	msg.addr = client->addr;
	msg.flags = client->flags;
	msg.len = count + 1;
	msg.buf = (char *)tx_buf;
	//msg.scl_rate = scl_rate;

	ret = i2c_transfer(adap, &msg, 1);
	kfree(tx_buf);
	return (ret == 1) ? count : ret;

}

static int i2c_master_reg8_recv(const struct i2c_client *client, const char reg, char *buf, int count, int scl_rate)
{
	struct i2c_adapter *adap=client->adapter;
	struct i2c_msg msgs[2];
	int ret;
	char reg_buf = reg;
	
	msgs[0].addr = client->addr;
	msgs[0].flags = client->flags;
	msgs[0].len = 1;
	msgs[0].buf = &reg_buf;
	//msgs[0].scl_rate = scl_rate;

	msgs[1].addr = client->addr;
	msgs[1].flags = client->flags | I2C_M_RD;
	msgs[1].len = count;
	msgs[1].buf = (char *)buf;
	//msgs[1].scl_rate = scl_rate;

	ret = i2c_transfer(adap, msgs, 2);

	return (ret == 2)? count : ret;
}



static int hym8563_i2c_read_regs(struct i2c_client *client, u8 reg, u8 buf[], unsigned len)
{
	int ret; 
	ret = i2c_master_reg8_recv(client, reg, buf, len, RTC_SPEED);
	return ret; 
}

static int hym8563_i2c_set_regs(struct i2c_client *client, u8 reg, u8 const buf[], __u16 len)
{
	int ret; 
	ret = i2c_master_reg8_send(client, reg, buf, (int)len, RTC_SPEED);
	return ret;
}


int hym8563_enable_count(struct i2c_client *client, int en)
{
	struct hym8563 *hym8563 = i2c_get_clientdata(client);	
	u8 regs[2];

	if (!hym8563)
		return -1;

	if (en) {
		hym8563_i2c_read_regs(client, RTC_CTL2, regs, 1);
		regs[0] |= TIE;
		hym8563_i2c_set_regs(client, RTC_CTL2, regs, 1);
		regs[0] = 0;
		regs[0] |= (TE | TD1);
		hym8563_i2c_set_regs(client, RTC_T_CTL, regs, 1);
	}
	else {
		hym8563_i2c_read_regs(client, RTC_CTL2, regs, 1);
		regs[0] &= ~TIE;
		hym8563_i2c_set_regs(client, RTC_CTL2, regs, 1);
		regs[0] = 0;
		regs[0] |= (TD0 | TD1);
		hym8563_i2c_set_regs(client, RTC_T_CTL, regs, 1);
	}
	return 0;
}

//0 < sec <=255
int hym8563_set_count(struct i2c_client *client, int sec)
{	
	struct hym8563 *hym8563 = i2c_get_clientdata(client);
	u8 regs[2];

	if (!hym8563)
		return -1;
		
	if (sec >= 255)
		regs[0] = 255;
	else if (sec <= 1)
		regs[0] = 1;
	else
		regs[0] = sec;
	
	hym8563_i2c_set_regs(client, RTC_T_COUNT, regs, 1);
	
	return 0;
}


/*the init of the hym8563 at first time */
static int hym8563_init_device(struct i2c_client *client)	
{
	struct hym8563 *hym8563 = i2c_get_clientdata(client);
	u8 regs[2];
	int sr;

	mutex_lock(&hym8563->mutex);
	regs[0]=0;
	sr = hym8563_i2c_set_regs(client, RTC_CTL1, regs, 1);		
	if (sr < 0)
		goto exit;
	
	//disable clkout
	regs[0] = 0x80;
	sr = hym8563_i2c_set_regs(client, RTC_CLKOUT, regs, 1);
	if (sr < 0)
		goto exit;

	/*enable alarm && count interrupt*/
	sr = hym8563_i2c_read_regs(client, RTC_CTL2, regs, 1);
	if (sr < 0)
		goto exit;
	regs[0] = 0x0;
	regs[0] |= (AIE | TIE);
	sr = hym8563_i2c_set_regs(client, RTC_CTL2, regs, 1);
	if (sr < 0)
		goto exit;
	sr = hym8563_i2c_read_regs(client, RTC_CTL2, regs, 1);
	if (sr < 0)
		goto exit;

	sr = hym8563_i2c_read_regs(client, RTC_CTL2, regs, 1);
	if (sr < 0) {
		pr_err("read CTL2 err\n");
		goto exit;
	}
	
	if(regs[0] & (AF|TF))
	{
		regs[0] &= ~(AF|TF);
		sr = hym8563_i2c_set_regs(client, RTC_CTL2, regs, 1);
	}
	
exit:
	mutex_unlock(&hym8563->mutex);
	
	return sr;
}

static int hym8563_read_datetime(struct i2c_client *client, struct rtc_time *tm)
{
	struct hym8563 *hym8563 = i2c_get_clientdata(client);
	u8 regs[HYM8563_RTC_SECTION_LEN] = { 0, };
	mutex_lock(&hym8563->mutex);
//	for (i = 0; i < HYM8563_RTC_SECTION_LEN; i++) {
//		hym8563_i2c_read_regs(client, RTC_SEC+i, &regs[i], 1);
//	}
	hym8563_i2c_read_regs(client, RTC_SEC, regs, HYM8563_RTC_SECTION_LEN);

	mutex_unlock(&hym8563->mutex);
	
	tm->tm_sec = bcd2bin(regs[0x00] & 0x7F);
	tm->tm_min = bcd2bin(regs[0x01] & 0x7F);
	tm->tm_hour = bcd2bin(regs[0x02] & 0x3F);
	tm->tm_mday = bcd2bin(regs[0x03] & 0x3F);
	tm->tm_wday = bcd2bin(regs[0x04] & 0x07);	
	
	tm->tm_mon = bcd2bin(regs[0x05] & 0x1F) ; 
	tm->tm_mon -= 1;			//inorder to cooperate the systerm time
	
	tm->tm_year = bcd2bin(regs[0x06] & 0xFF);
	if(regs[5] & 0x80)
		tm->tm_year += 1900;
	else
		tm->tm_year += 2000;
		
	tm->tm_yday = rtc_year_days(tm->tm_mday, tm->tm_mon, tm->tm_year);	
	
	tm->tm_year -= 1900;			//inorder to cooperate the systerm time	
	if(tm->tm_year < 0)
		tm->tm_year = 0;	
	tm->tm_isdst = 0;	

	pr_debug("%4d-%02d-%02d(%d) %02d:%02d:%02d\n",
		1900 + tm->tm_year, tm->tm_mon + 1, tm->tm_mday, tm->tm_wday,
		tm->tm_hour, tm->tm_min, tm->tm_sec);

	return 0;
}

static int hym8563_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	return hym8563_read_datetime(to_i2c_client(dev), tm);
}

static int hym8563_set_time(struct i2c_client *client, struct rtc_time *tm)	
{
	struct hym8563 *hym8563 = i2c_get_clientdata(client);
	u8 regs[HYM8563_RTC_SECTION_LEN] = { 0, };
	u8 mon_day;
	//u8 ret = 0;

	pr_debug("%4d-%02d-%02d(%d) %02d:%02d:%02d\n",
		1900 + tm->tm_year, tm->tm_mon + 1, tm->tm_mday, tm->tm_wday,
		tm->tm_hour, tm->tm_min, tm->tm_sec);

	mon_day = rtc_month_days((tm->tm_mon), tm->tm_year + 1900);
	
	if(tm->tm_sec >= 60 || tm->tm_sec < 0 )		//set  sec
		regs[0x00] = bin2bcd(0x00);
	else
		regs[0x00] = bin2bcd(tm->tm_sec);
	
	if(tm->tm_min >= 60 || tm->tm_min < 0 )		//set  min	
		regs[0x01] = bin2bcd(0x00);
	else
		regs[0x01] = bin2bcd(tm->tm_min);

	if(tm->tm_hour >= 24 || tm->tm_hour < 0 )		//set  hour
		regs[0x02] = bin2bcd(0x00);
	else
		regs[0x02] = bin2bcd(tm->tm_hour);
	
	if((tm->tm_mday) > mon_day)				//if the input month day is bigger than the biggest day of this month, set the biggest day 
		regs[0x03] = bin2bcd(mon_day);
	else if((tm->tm_mday) > 0)
		regs[0x03] = bin2bcd(tm->tm_mday);
	else if((tm->tm_mday) <= 0)
		regs[0x03] = bin2bcd(0x01);

	if( tm->tm_year >= 200)		// year >= 2100
		regs[0x06] = bin2bcd(99);	//year = 2099
	else if(tm->tm_year >= 100)			// 2000 <= year < 2100
		regs[0x06] = bin2bcd(tm->tm_year - 100);
	else if(tm->tm_year >= 0){				// 1900 <= year < 2000
		regs[0x06] = bin2bcd(tm->tm_year);	
		regs[0x05] |= 0x80;	
	}else{									// year < 1900
		regs[0x06] = bin2bcd(0);	//year = 1900	
		regs[0x05] |= 0x80;	
	}	
	regs[0x04] = bin2bcd(tm->tm_wday);		//set  the  weekday
	regs[0x05] = (regs[0x05] & 0x80)| (bin2bcd(tm->tm_mon + 1) & 0x7F);		//set  the  month
	
	mutex_lock(&hym8563->mutex);
//	for(i=0;i<HYM8563_RTC_SECTION_LEN;i++){
//		ret = hym8563_i2c_set_regs(client, RTC_SEC+i, &regs[i], 1);
//	}
	hym8563_i2c_set_regs(client, RTC_SEC, regs, HYM8563_RTC_SECTION_LEN);

	mutex_unlock(&hym8563->mutex);

	return 0;
}

static int hym8563_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	return hym8563_set_time(to_i2c_client(dev), tm);
}

static int hym8563_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *tm)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct hym8563 *hym8563 = i2c_get_clientdata(client);
	u8 regs[4] = { 0, };
	
	pr_debug("enter\n");
	mutex_lock(&hym8563->mutex);
	hym8563_i2c_read_regs(client, RTC_A_MIN, regs, 4);
	regs[0] = 0x0;
	regs[0] |= TIE;
	hym8563_i2c_set_regs(client, RTC_CTL2, regs, 1);
	mutex_unlock(&hym8563->mutex);
	return 0;
}

static int hym8563_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alarm)
{	
	struct i2c_client *client = to_i2c_client(dev);
	struct hym8563 *hym8563 = i2c_get_clientdata(client);
	struct rtc_time now, *tm = &alarm->time;
	u8 regs[4] = { 0, };
	u8 mon_day;	
	unsigned long	alarm_sec, now_sec;
	int diff_sec = 0;
	
	printk("%4d-%02d-%02d(%d) %02d:%02d:%02d enabled %d\n",
		1900 + tm->tm_year, tm->tm_mon + 1, tm->tm_mday, tm->tm_wday,
		tm->tm_hour, tm->tm_min, tm->tm_sec, alarm->enabled);
	
	
	hym8563_read_datetime(client, &now);

	
	mutex_lock(&hym8563->mutex);
	rtc_tm_to_time(tm, &alarm_sec);
	rtc_tm_to_time(&now, &now_sec);
	
	diff_sec = alarm_sec - now_sec;
	
	if((diff_sec > 0) && (diff_sec < 256))
	{	
		printk("%s:diff_sec= %ds , use time\n",__func__, diff_sec);	
								
		if (alarm->enabled == 1)
		{
			hym8563_set_count(client, diff_sec);
			hym8563_enable_count(client, 1);
		}
			
		else
		{
			hym8563_enable_count(client, 0);
		}
		
	}
	else
	{				
		printk("%s:diff_sec= %ds , use alarm\n",__func__, diff_sec);
		hym8563_enable_count(client, 0);
		
		if(tm->tm_sec > 0)
		{
			rtc_tm_to_time(tm, &alarm_sec);
			rtc_time_to_tm(alarm_sec, tm);
		}

		hym8563->alarm = *alarm;

		regs[0] = 0x0;
		hym8563_i2c_set_regs(client, RTC_CTL2, regs, 1);
		mon_day = rtc_month_days(tm->tm_mon, tm->tm_year + 1900);
		hym8563_i2c_read_regs(client, RTC_A_MIN, regs, 4);

		if (tm->tm_min >= 60 || tm->tm_min < 0)		//set  min
		regs[0x00] = bin2bcd(0x00) & 0x7f;
		else
		regs[0x00] = bin2bcd(tm->tm_min) & 0x7f;
		if (tm->tm_hour >= 24 || tm->tm_hour < 0)	//set  hour
		regs[0x01] = bin2bcd(0x00) & 0x7f;
		else
		regs[0x01] = bin2bcd(tm->tm_hour) & 0x7f;
		regs[0x03] = bin2bcd (tm->tm_wday) & 0x7f;

		/* if the input month day is bigger than the biggest day of this month, set the biggest day */
		if (tm->tm_mday > mon_day)
		regs[0x02] = bin2bcd(mon_day) & 0x7f;
		else if (tm->tm_mday > 0)
		regs[0x02] = bin2bcd(tm->tm_mday) & 0x7f;
		else if (tm->tm_mday <= 0)
		regs[0x02] = bin2bcd(0x01) & 0x7f;

		hym8563_i2c_set_regs(client, RTC_A_MIN, regs, 4);	
		hym8563_i2c_read_regs(client, RTC_A_MIN, regs, 4);	
		hym8563_i2c_read_regs(client, RTC_CTL2, regs, 1);
		if (alarm->enabled == 1)
		regs[0] |= AIE;
		else
		regs[0] &= 0x0;
		hym8563_i2c_set_regs(client, RTC_CTL2, regs, 1);
		hym8563_i2c_read_regs(client, RTC_CTL2, regs, 1);
		
		printk("alarm regs[0]=0x%x\n",regs[0]);
		
		if(diff_sec <= 0)
		{		
			pr_info("alarm sec  <= now sec\n");
		}			

	}
	
	mutex_unlock(&hym8563->mutex);

	return 0;
}

static int xh_rtc_set_alarm(struct rtc_wkalrm *alarm)
{	
	struct i2c_client *client = gClient;
	struct hym8563 *hym8563 = g_hym8563;
	struct rtc_time now, *tm = &alarm->time;
	u8 regs[4] = { 0, };
	u8 mon_day;	
	unsigned long	alarm_sec, now_sec;
	int diff_sec = 0;
	
	printk("%4d-%02d-%02d(%d) %02d:%02d:%02d enabled %d\n",
		1900 + tm->tm_year, tm->tm_mon + 1, tm->tm_mday, tm->tm_wday,
		tm->tm_hour, tm->tm_min, tm->tm_sec, alarm->enabled);
	
	
	hym8563_read_datetime(client, &now);

	
	mutex_lock(&hym8563->mutex);
	rtc_tm_to_time(tm, &alarm_sec);
	rtc_tm_to_time(&now, &now_sec);
	
	diff_sec = alarm_sec - now_sec;
	
	if((diff_sec > 0) && (diff_sec < 256))
	{	
		printk("%s:diff_sec= %ds , use time\n",__func__, diff_sec);	
								
		if (alarm->enabled == 1)
		{
			hym8563_set_count(client, diff_sec);
			hym8563_enable_count(client, 1);
		}
			
		else
		{
			hym8563_enable_count(client, 0);
		}
		
	}
	else
	{				
		printk("%s:diff_sec= %ds , use alarm\n",__func__, diff_sec);
		hym8563_enable_count(client, 0);
		
		if(tm->tm_sec > 0)
		{
			rtc_tm_to_time(tm, &alarm_sec);
			rtc_time_to_tm(alarm_sec, tm);
		}

		hym8563->alarm = *alarm;

		regs[0] = 0x0;
		hym8563_i2c_set_regs(client, RTC_CTL2, regs, 1);
		mon_day = rtc_month_days(tm->tm_mon, tm->tm_year + 1900);
		hym8563_i2c_read_regs(client, RTC_A_MIN, regs, 4);

		if (tm->tm_min >= 60 || tm->tm_min < 0)		//set  min
		regs[0x00] = bin2bcd(0x00) & 0x7f;
		else
		regs[0x00] = bin2bcd(tm->tm_min) & 0x7f;
		if (tm->tm_hour >= 24 || tm->tm_hour < 0)	//set  hour
		regs[0x01] = bin2bcd(0x00) & 0x7f;
		else
		regs[0x01] = bin2bcd(tm->tm_hour) & 0x7f;
		regs[0x03] = bin2bcd (tm->tm_wday) & 0x7f;

		/* if the input month day is bigger than the biggest day of this month, set the biggest day */
		if (tm->tm_mday > mon_day)
		regs[0x02] = bin2bcd(mon_day) & 0x7f;
		else if (tm->tm_mday > 0)
		regs[0x02] = bin2bcd(tm->tm_mday) & 0x7f;
		else if (tm->tm_mday <= 0)
		regs[0x02] = bin2bcd(0x01) & 0x7f;

		hym8563_i2c_set_regs(client, RTC_A_MIN, regs, 4);	
		hym8563_i2c_read_regs(client, RTC_A_MIN, regs, 4);	
		hym8563_i2c_read_regs(client, RTC_CTL2, regs, 1);
		if (alarm->enabled == 1)
		regs[0] |= AIE;
		else
		regs[0] &= 0x0;
		hym8563_i2c_set_regs(client, RTC_CTL2, regs, 1);
		hym8563_i2c_read_regs(client, RTC_CTL2, regs, 1);
		
		printk("alarm regs[0]=0x%x\n",regs[0]);
		
		if(diff_sec <= 0)
		{		
			pr_info("alarm sec  <= now sec\n");
		}			

	}
	
	mutex_unlock(&hym8563->mutex);

	return 0;
}
#ifdef CONFIG_HDMI_SAVE_DATA
int hdmi_get_data(void)
{
    u8 regs=0;
    if(gClient)
        hym8563_i2c_read_regs(gClient, RTC_T_COUNT, &regs, 1);
    else 
    {
        printk("%s rtc has no init\n",__func__);
        return -1;
    }
    if(regs==0 || regs==0xff){
        printk("%s rtc has no hdmi data\n",__func__);
        return -1;
    }
    return (regs-1);
}

int hdmi_set_data(int data)
{
    u8 regs = (data+1)&0xff;
    if(gClient)
        hym8563_i2c_set_regs(gClient, RTC_T_COUNT, &regs, 1);
    else 
    {
        printk("%s rtc has no init\n",__func__);
        return -1;
    }   
    return 0;
}

EXPORT_SYMBOL(hdmi_get_data);
EXPORT_SYMBOL(hdmi_set_data);
#endif
#if defined(CONFIG_RTC_INTF_DEV) || defined(CONFIG_RTC_INTF_DEV_MODULE)
static int hym8563_i2c_open_alarm(struct i2c_client *client)
{
	u8 data;	
	hym8563_i2c_read_regs(client, RTC_CTL2, &data, 1);
	data |= AIE;
	hym8563_i2c_set_regs(client, RTC_CTL2, &data, 1);

	return 0;
}

static int hym8563_i2c_close_alarm(struct i2c_client *client)
{
	u8 data;	
	hym8563_i2c_read_regs(client, RTC_CTL2, &data, 1);
	data &= ~AIE;
	hym8563_i2c_set_regs(client, RTC_CTL2, &data, 1);

	return 0;
}

static int hym8563_rtc_ioctl(struct device *dev, unsigned int cmd, unsigned long arg)
{
	struct i2c_client *client = to_i2c_client(dev);

	switch (cmd) {
	case RTC_AIE_OFF:
		if(hym8563_i2c_close_alarm(client) < 0)
			goto err;
		break;
	case RTC_AIE_ON:
		if(hym8563_i2c_open_alarm(client))
			goto err;
		break;
	default:
		return -ENOIOCTLCMD;
	}	
	return 0;
err:
	return -EIO;
}
#else
#define hym8563_rtc_ioctl NULL
#endif

#if defined(CONFIG_RTC_INTF_PROC) || defined(CONFIG_RTC_INTF_PROC_MODULE)
static int hym8563_rtc_proc(struct device *dev, struct seq_file *seq)
{
	return 0;
}
#else
#define hym8563_rtc_proc NULL
#endif

int xh_rtc_cancle_alarm(void)
{
	u8 value;
	
	printk("xh_rtc_cancle_alarm\n");
	
	//mutex_lock(&g_hym8563->mutex);
	
	hym8563_enable_count(gClient, 0);
	hym8563_i2c_read_regs(gClient, RTC_CTL2, &value, 1);
	value &= ~(AF|TF);
	value &= 0x0;
	hym8563_i2c_set_regs(gClient, RTC_CTL2, &value, 1);
	hym8563_i2c_read_regs(gClient, RTC_CTL2, &value, 1);
	//printk("alarm value=0x%x\n",value);
	
	//mutex_unlock(&g_hym8563->mutex);
		
	return 0;
}EXPORT_SYMBOL(xh_rtc_cancle_alarm);

static int hym8563_rtc_alarm_irq_enable(struct device *dev,
				       unsigned int enabled)
{
		return 0;
}
static irqreturn_t hym8563_wakeup_irq(int irq, void *data)
{
	struct hym8563 *hym8563 = data;	
	struct i2c_client *client = hym8563->client;	
	u8 value;
	
	mutex_lock(&hym8563->mutex);
	
	hym8563_enable_count(client, 0);
	
	hym8563_i2c_read_regs(client, RTC_CTL2, &value, 1);
	value &= ~(AF|TF);
	hym8563_i2c_set_regs(client, RTC_CTL2, &value, 1);	
	mutex_unlock(&hym8563->mutex);
	
	rtc_update_irq(hym8563->rtc, 1, RTC_IRQF | RTC_AF | RTC_UF);

	printk("%s:irq=%d\n",__func__,irq);
	return IRQ_HANDLED;
}
#ifdef CONFIG_COMMON_CLK
#define clkout_hw_to_hym8563(_hw) container_of(_hw, struct hym8563, clkout_hw)

static int clkout_rates[] = {
	32768,
	1024,
	32,
	1,
};

static unsigned long hym8563_clkout_recalc_rate(struct clk_hw *hw,
						unsigned long parent_rate)
{
	struct hym8563 *hym8563 = clkout_hw_to_hym8563(hw);
	struct i2c_client *client = hym8563->client;
	u8 ret;
	
	hym8563_i2c_read_regs(client, HYM8563_CLKOUT, &ret, 1);

	if (ret < 0)
		return 0;

	ret &= HYM8563_CLKOUT_MASK;
	return clkout_rates[ret];
}

static long hym8563_clkout_round_rate(struct clk_hw *hw, unsigned long rate,
				      unsigned long *prate)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(clkout_rates); i++)
		if (clkout_rates[i] <= rate)
			return clkout_rates[i];

	return 0;
}

static int hym8563_clkout_set_rate(struct clk_hw *hw, unsigned long rate,
				   unsigned long parent_rate)
{
	struct hym8563 *hym8563 = clkout_hw_to_hym8563(hw);
	struct i2c_client *client = hym8563->client;
	u8 ret;
	int i;

	hym8563_i2c_read_regs(client, HYM8563_CLKOUT, &ret, 1);	
	if (ret < 0)
		return ret;

	for (i = 0; i < ARRAY_SIZE(clkout_rates); i++)
		if (clkout_rates[i] == rate) {
			ret &= ~HYM8563_CLKOUT_MASK;
			ret |= i;
			hym8563_i2c_set_regs(client,HYM8563_CLKOUT, &ret,1);
			return 0;
		}

	return -EINVAL;
}

static int hym8563_clkout_control(struct clk_hw *hw, bool enable)
{
	struct hym8563 *hym8563 = clkout_hw_to_hym8563(hw);
	struct i2c_client *client = hym8563->client;
	u8 ret;

	hym8563_i2c_read_regs(client, HYM8563_CLKOUT, &ret, 1);	
	
	if (ret < 0)
		return ret;

	if (enable)
		ret |= HYM8563_CLKOUT_ENABLE;
	else
		ret &= ~HYM8563_CLKOUT_ENABLE;
	hym8563_i2c_set_regs(client, HYM8563_CLKOUT,  &ret, 1);
	return 0;
}

static int hym8563_clkout_prepare(struct clk_hw *hw)
{
	return hym8563_clkout_control(hw, 1);
}

static void hym8563_clkout_unprepare(struct clk_hw *hw)
{
	hym8563_clkout_control(hw, 0);
}

static int hym8563_clkout_is_prepared(struct clk_hw *hw)
{
	struct hym8563 *hym8563 = clkout_hw_to_hym8563(hw);
	struct i2c_client *client = hym8563->client;
	u8 ret;

	hym8563_i2c_read_regs(client, HYM8563_CLKOUT, &ret, 1);	

	if (ret < 0)
		return ret;

	return !!(ret & HYM8563_CLKOUT_ENABLE);
}

static const struct clk_ops hym8563_clkout_ops = {
	.prepare = hym8563_clkout_prepare,
	.unprepare = hym8563_clkout_unprepare,
	.is_prepared = hym8563_clkout_is_prepared,
	.recalc_rate = hym8563_clkout_recalc_rate,
	.round_rate = hym8563_clkout_round_rate,
	.set_rate = hym8563_clkout_set_rate,
};

static struct clk *hym8563_clkout_register_clk(struct hym8563 *hym8563)
{
	struct i2c_client *client = hym8563->client;
	struct device_node *node = client->dev.of_node;
	struct clk *clk;
	struct clk_init_data init;
	int ret;
	u8 value;
	
	value=0;
	
	ret = hym8563_i2c_set_regs(client, HYM8563_CLKOUT,  &value, 1);
	
	if (ret < 0)
		return ERR_PTR(ret);

	init.name = "hym8563-clkout";
	init.ops = &hym8563_clkout_ops;
	init.flags = CLK_IS_ROOT;
	init.parent_names = NULL;
	init.num_parents = 0;
	hym8563->clkout_hw.init = &init;

	/* optional override of the clockname */
	of_property_read_string(node, "clock-output-names", &init.name);

	/* register the clock */
	clk = clk_register(&client->dev, &hym8563->clkout_hw);

	if (!IS_ERR(clk))
		of_clk_add_provider(node, of_clk_src_simple_get, clk);

	return clk;
}
#endif
static int xhrtc_open(struct inode *inode, struct file *filp)
{
    return 0;
}

static int xhrtc_release(struct inode *inode, struct file *filp)
{
    return 0;
}

static long xhrtc_compat_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    int err = 0;
    struct timespec ts;
		struct rtc_time time;
		struct rtc_wkalrm alarm;  
		
    switch (cmd) 
    {
    		 case XHRTC_SET_ALARM:
	     		break;  
	     		
	     	case XHRTC_CANALE_ALARM:
	     		xh_rtc_cancle_alarm();
	     		return err; 
	     			     		 
	    	default:
	        pr_err("Invalid ioctl command.\n");
	        return -ENOTTY;
    }		  
    
    if (copy_from_user(&ts, (void __user *)arg, sizeof(ts)))
			return -EFAULT;

		printk("jessica xhrtc ts.tv_sec=%d\n",ts.tv_sec);
				
		rtc_time_to_tm(ts.tv_sec,&time);
		alarm.time.tm_year = time.tm_year;
		alarm.time.tm_mon = time.tm_mon;
		alarm.time.tm_mday = time.tm_mday;
		alarm.time.tm_wday = time.tm_wday;
		alarm.time.tm_hour = time.tm_hour;
		alarm.time.tm_min = time.tm_min;
		alarm.time.tm_sec = time.tm_sec;
		alarm.enabled = 1;
		    
    switch (cmd) 
    {
	     	case XHRTC_SET_ALARM:
					xh_rtc_set_alarm(&alarm);
	     		break;  
    			     		 
	    	default:
	        pr_err("Invalid ioctl command.\n");
	        return -ENOTTY;
    }
    
    return err;
}
static const struct rtc_class_ops hym8563_rtc_ops = {
	.read_time	= hym8563_rtc_read_time,
	.set_time	= hym8563_rtc_set_time,
	.read_alarm	= hym8563_rtc_read_alarm,
	.set_alarm	= hym8563_rtc_set_alarm,
	.ioctl 		= hym8563_rtc_ioctl,
	.alarm_irq_enable = hym8563_rtc_alarm_irq_enable,
	.proc		= hym8563_rtc_proc
};

static const struct file_operations xhrtc_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = xhrtc_compat_ioctl,
    .open = xhrtc_open,
    .release = xhrtc_release
};
static struct miscdevice xhrtc_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "xh_rtc",
    .fops = &xhrtc_fops
};

static int  hym8563_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	int rc = 0;
	u8 reg = 0;
	struct hym8563 *hym8563;
	struct rtc_wkalrm alarm;
	struct rtc_device *rtc = NULL;
	struct rtc_time tm_read, tm = {
		.tm_wday = 6,
		.tm_year = 111,
		.tm_mon = 0,
		.tm_mday = 1,
		.tm_hour = 12,
		.tm_min = 0,
		.tm_sec = 0,
	};	

	struct device_node *np = client->dev.of_node;
	unsigned long irq_flags;
	int result;
	
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -ENODEV;
		
	hym8563 = devm_kzalloc(&client->dev,sizeof(*hym8563), GFP_KERNEL);
	if (!hym8563) {
		return -ENOMEM;
	}

	gClient = client;	
	hym8563->client = client;
	hym8563->alarm.enabled = 0;
	client->irq = 0;
	mutex_init(&hym8563->mutex);
	wake_lock_init(&hym8563->wake_lock, WAKE_LOCK_SUSPEND, "rtc_hym8563");
	i2c_set_clientdata(client, hym8563);

	hym8563_init_device(client);	
	hym8563_enable_count(client, 0);	
	device_set_wakeup_capable(&client->dev, true);
	// check power down 
	hym8563_i2c_read_regs(client,RTC_SEC,&reg,1);
	if (reg&0x80) {
		dev_info(&client->dev, "clock/calendar information is no longer guaranteed\n");
		hym8563_set_time(client, &tm);
	}

	hym8563_read_datetime(client, &tm_read);	//read time from hym8563
	
	if(((tm_read.tm_year < 70) | (tm_read.tm_year > 137 )) | (tm_read.tm_mon == -1) | (rtc_valid_tm(&tm_read) != 0)) //if the hym8563 haven't initialized
	{
		hym8563_set_time(client, &tm);	//initialize the hym8563 
	}	
	
	client->irq = of_get_named_gpio_flags(np, "irq_gpio", 0,(enum of_gpio_flags *)&irq_flags);
	if(client->irq >= 0)
        {
	        hym8563->irq = gpio_to_irq(client->irq);
	        result = devm_request_threaded_irq(&client->dev, hym8563->irq, NULL, hym8563_wakeup_irq, irq_flags | IRQF_ONESHOT, client->dev.driver->name,hym8563 );
	        if (result) {
		        printk(KERN_ERR "%s:fail to request irq = %d, ret = 0x%x\n",__func__, hym8563->irq, result);
		        goto exit;
	        }
	        enable_irq_wake(hym8563->irq);
	        
        }
		device_init_wakeup(&client->dev, 1);
	rtc = devm_rtc_device_register(&client->dev,
			client->name,
                       	&hym8563_rtc_ops, THIS_MODULE);
	if (IS_ERR(rtc)) {
		rc = PTR_ERR(rtc);
		rtc = NULL;
		goto exit;
	}
	hym8563->rtc = rtc;
	
	g_hym8563 = hym8563;
  hym8563_rtc_read_alarm(&gClient->dev,&alarm);
  
  misc_register(&xhrtc_dev);	 
  
  #ifdef CONFIG_COMMON_CLK
	hym8563_clkout_register_clk(hym8563);
	#endif
	
	return 0;

exit:
	if (hym8563) {
		wake_lock_destroy(&hym8563->wake_lock);
	}
	return rc;
}

static int  hym8563_remove(struct i2c_client *client)
{
	struct hym8563 *hym8563 = i2c_get_clientdata(client);

	wake_lock_destroy(&hym8563->wake_lock);

	return 0;
}


static void hym8563_shutdown(struct i2c_client * client)
{
  //struct device *pdev = &client->dev;
  struct rtc_wkalrm alarm ;
  struct rtc_device *rtc_dev=g_hym8563->rtc; 
  u8 regs[4];   
    
  rtc_read_alarm(rtc_dev,&alarm);

  hym8563_i2c_read_regs(client, RTC_CTL2, regs, 1);
  regs[0] |= AIE;
  hym8563_i2c_set_regs(client, RTC_CTL2, regs, 1);
  hym8563_i2c_read_regs(client, RTC_CTL2, regs, 1);

  regs[0] |= TIE;
  hym8563_i2c_set_regs(client, RTC_CTL2, regs, 1);
  hym8563_i2c_read_regs(client, RTC_CTL2, regs, 1);
  
  if(alarm.enabled == 1){
       printk("%s this in shutdowm,the alarm.enabled =1 \n",__func__);
	}  
	
}



static const struct i2c_device_id hym8563_id[] = {
	{ "hym8563", 0 },
	{},
};
MODULE_DEVICE_TABLE(i2c, hym8563_id);

static const struct of_device_id hym8563_dt_idtable[] = {
	{ .compatible = "haoyu,hym8563" },
	{},
};
MODULE_DEVICE_TABLE(of, hym8563_dt_idtable);

static struct i2c_driver hym8563_driver = {
	.driver		= {
		.name	= "rtc-hym8563",

		.of_match_table	= hym8563_dt_idtable,
	},
	.probe		= hym8563_probe,
	.remove		= hym8563_remove,
	.shutdown=hym8563_shutdown,
	.id_table	= hym8563_id,
};

static int __init hym8563_init(void)
{
	return i2c_add_driver(&hym8563_driver);
}

static void __exit hym8563_exit(void)
{
	i2c_del_driver(&hym8563_driver);
}

MODULE_AUTHOR("lhh lhh@rock-chips.com");
MODULE_DESCRIPTION("HYM8563 RTC driver");
MODULE_LICENSE("GPL");

module_init(hym8563_init);
module_exit(hym8563_exit);

