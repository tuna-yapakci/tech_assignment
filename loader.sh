#!/bin/sh
module="driver"
module2="driver2"
device="gpio_master"
device2="gpio_slave"

cp ${module}.ko ${module2}.ko

/sbin/insmod ${module}.ko gpio_pin_number=22 comm_role=0 || exit 1
/sbin/insmod ${module2}.ko gpio_pin_number=17 comm_role=1 || exit 1

rm -f /dev/${device}
rm -f /dev/${device2}

major=`cat /proc/devices | awk "{if(\\$2==\"$device\")print \\$1}"`
major2=`cat /proc/devices | awk "{if(\\$2==\"$device2\")print \\$1}"`

mknod /dev/${device} c $major 0
mknod /dev/${device2} c $major2 0

chmod 666 /dev/${device}
chmod 666 /dev/${device2}