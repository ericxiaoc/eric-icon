#!/bin/bash
LINUXROOT=./linuxroot.img

ubuntu_order=`sudo du -m --max-depth=0 export_rootfs/`;
ubuntu_size=$((`echo "$ubuntu_order" | awk '{print $1}'`+200));
echo $ubuntu_size

sudo rm -rf rootfs/*
if [ ! -f "$LINUXROOT" ];then
	echo "$LINUXROOT is not exits"
else
	rm -rf $LINUXROOT 
fi
dd if=/dev/zero of=linuxroot.img bs=1M count=$ubuntu_size
mkfs.ext4 -F -L linuxroot $LINUXROOT

sudo mount $LINUXROOT ./rootfs

cp -rfp ./export_rootfs/*  ./rootfs

sudo umount ./rootfs

e2fsck -p -f $LINUXROOT
resize2fs -M $LINUXROOT
ls -lh $LINUXROOT



