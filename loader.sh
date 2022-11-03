#!/bin/sh
module="driver"
device="custom_gpio_dev"
mode="664"

/sbin/insmod ${module}.ko gpio_pin_number=22 || exit 1


rm -f /dev/${device}

major=`cat /proc/devices | awk "{if(\\$2==\"$device\")print \\$1}"`

mknod /dev/${device} c $major 0
