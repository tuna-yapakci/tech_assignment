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
#include <asm/uaccess.h>
MODULE_LICENSE("GPL");

#define GPIO_21 (21)

struct gpio_dev {
    struct cdev cdev;
};

static dev_t dev = 0;
static struct class *dev_class;
struct gpio_dev g_dev;

static ssize_t gpio_read(struct file *filp, char __user *buff, size_t count, loff_t *offp);
static ssize_t gpio_write(struct file *filp, const char __user *buff, size_t count, loff_t *offp);
static int gpio_open(struct inode *inode, struct file *file);
static int gpio_close(struct inode *inode, struct file *file);

static struct file_operations gpio_fops = {
    .owner = THIS_MODULE,
    .open = gpio_open,
    .release = gpio_close,
    .read = gpio_read,
    .write = gpio_write,
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
static void cleanup_func(void){
    if(chrdev_allocated) {
        unregister_chrdev_region(dev,1);
    }
    if(device_registered) {
        cdev_del(&(g_dev.cdev));
    }
}



static int gpio_open(struct inode *inode, struct file *file){
    printk("Device file opened\n");
    return 0;
}

static int gpio_close(struct inode *inode, struct file *file){
    printk("Device file closed");
    return 0;
}

static ssize_t gpio_read(struct file *filp, char __user *buff, size_t count, loff_t *offp){
    uint8_t gpio_state = gpio_get_value(GPIO_21);
    
    count = 1;
    if(copy_to_user(buff, &gpio_state, len) > 0) {
        printk(KERN_WARNING "ERROR");
    }

    printk(KERN_INFO "GPIO_21 state = %d \n", gpio_state);
    return 0;
}
static ssize_t gpio_write(struct file *filp, const char __user *buff, size_t count, loff_t *offp){
    uint8_t rec_buff[10] = {0};

    if(copy_from_user(rec_buff, buff, count) > 0) {
        printk(KERN_WARNING "ERROR1");
    }

    if (rec_buff[0]=='1'){
        gpio_set_value(GPIO_21, 1);
    }
    else if (rec_buff[0]=='0'){
        gpio_set_value(GPIO_21, 0);
    }
    else {
        printk(KERN_WARNING "ERROR2");
    }

    return count;
}


static int __init gpio_driver_init(void){
    // TODO check if the given number is valid
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

    if(gpio_is_valid(GPIO_21) == false){
        printk(KERN_WARNING "Invalid GPIO\n");
        cleanup_func();
        return -1;
    }

    if(gpio_request(GPIO_21, "GPIO_21") < 0) {
        printk(KERN_WARNING "GPIO request error\n");
        cleanup_func();
        return -1;
    }

    gpio_direction_output(GPIO_21, 0); //TODO change this

    gpio_export(GPIO_21, false);

    printk("Driver loaded\n");
    return 0;
}

static void __exit gpio_driver_exit(void){
    cleanup_func();
    printk("Driver removed\n");
}

module_init(gpio_driver_init);
module_exit(gpio_driver_exit);
