#!/bin/sh
module="driver"
device="custom_gpio_dev"
mode="664"

/sbin/insmod ./${module}.ko || exit 1

rm -f /dev/${device}

major=`cat /proc/devices | awk "{if(\\$2==\"$device\")print \\$1}"`

echo $major
