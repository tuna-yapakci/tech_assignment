#!/bin/sh
module="driver"
device="custom_gpio_dev"
mode="664"

/sbin/insmod ./${module}.ko || exit 1

rm -f /dev/${device}

major=$(awk "\\$2= =\"$module\" {print \\$1}" /proc/devices)

echo $major
