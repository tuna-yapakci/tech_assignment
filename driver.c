#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/stat.h>
#include <linux/sched.h>
#include <linux/cdev.h>
#include <linux/gpio.h>
MODULE_LICENSE("GPL");

struct gpio_dev {
    struct cdev cdev;
};

static dev_t dev = 0;
struct gpio_dev g_dev;

static int gpio_open(struct inode *inode, struct file *file);

static struct file_operations gpio_fops = {
    .owner = THIS_MODULE,
    .open = gpio_open,
    //.release = gpio_release,
    //.read = gpio_read,
    //.write = gpio_write,
};

static int gpio_setup_cdev(struct gpio_dev *g_dev){
    cdev_init(&g_dev->cdev, &gpio_fops);
    g_dev->cdev.owner = THIS_MODULE;
    g_dev->cdev.ops = &gpio_fops;
    if(cdev_add(&g_dev->cdev, dev, 1) < 0){
        return -1;
    }
    return 0;
}

static int gpio_pin_number = -1;
//enables indicating the pin number during initialization
//S_IRUGO means the parameter can be read but cannot be changed
module_param(gpio_pin_number, int, S_IRUGO);

//cleanup helper variables, useful for error handling
static int chrdev_allocated = 0;
static int device_registered = 0; //TODO rename these

//self explanatory
static void cleanup_func(){
    if(chrdev_allocated) {
        unregister_chrdev_region(dev,1);
    }
    if(device_registered) {
        cdev_del(&(g_dev->cdev));
    }
}

static int gpio_open(struct inode *inode, struct file *file){
    printk("Device file opened\n");
    return 0;
}

static int __init driver_init(){
    printk("The chosen pin is %d\n", gpio_pin_number);
    
    chrdev_allocated = 1;
    if(alloc_chrdev_region(&dev, 0, 1, "custom_gpio_dev") < 0) {
        printk(KERN_WARNING "Error during dev number allocation\n");
        cleanup_func();
        return -1;
    }
    printk(KERN_INFO "major number = %d, minor number = %d\n", MAJOR(dev), MINOR(dev));
    
    device_registered = 1;
    if(gpio_setup_cdev(&g_dev) < 0){
        printk(KERN_WARNING "Error adding device\n");
        cleanup_func();
        return -1;
    }

    printk("Driver loaded\n");
    return 0;
}

static void __exit driver_exit(){
    cleanup_func();
    printk("Driver removed\n");
}

module_init(driver_init);
module_exit(driver_exit);
