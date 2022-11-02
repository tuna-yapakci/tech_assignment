#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/stat.h>
#include <linux/sched.h>
MODULE_LICENSE("GPL");

dev_t dev = 0;

static int gpio_pin_number = -1;
//enables indicating the pin number during initialization
//S_IRUGO means the parameter can be read but cannot be changed
module_param(gpio_pin_number, int, S_IRUGO);

static int __init driver_init(void){
    printk(KERN_ALERT "Driver loaded\n");
    printk("The given number is %d", gpio_pin_number);
    return 0;
}

static void __exit driver_exit(void){
    printk(KERN_ALERT "Driver removed\n");
}

module_init(driver_init);
module_exit(driver_exit);

//token : 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