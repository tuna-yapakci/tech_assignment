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
#include <linux/ioctl.h>
#include <linux/pid.h>
#include <linux/delay.h>
#include <linux/kthread.h>
MODULE_LICENSE("GPL");

#define MAGIC 'k'
#define USER_APP_REG _IOW(MAGIC, 1, int*)
#define START_COMMS _IO(MAGIC, 2)
#define SIGDATARECV 47

//--------------------Prototypes and Structures--------------------

static ssize_t gpio_read(struct file *filp, char __user *buff, size_t count, loff_t *offp);
static ssize_t gpio_write(struct file *filp, const char __user *buff, size_t count, loff_t *offp);
static int gpio_open(struct inode *inode, struct file *file);
static int gpio_close(struct inode *inode, struct file *file);
static long gpioctl(struct file *filp, unsigned int cmd, unsigned long arg);

struct gpio_dev {
    struct cdev cdev;
};

static struct file_operations gpio_fops = {
    .owner = THIS_MODULE,
    .open = gpio_open,
    .release = gpio_close,
    .read = gpio_read,
    .write = gpio_write,
    .unlocked_ioctl = gpioctl,
};


//This part implements a circular fifo queue
static const int queue_size = 5;

struct Data {
    int length;
    char buffer[10];
};

struct DataQueue {
    int first_pos;
    int data_count;
    struct Data *array_pt;
};

static int data_queue_init(struct DataQueue *queue){
    queue->first_pos = 0;
    queue->data_count = 0;
    queue->array_pt = kmalloc(sizeof(struct Data)*queue_size, GFP_KERNEL);
    if(queue->array_pt == NULL) {
        return -1;
    }
    return 0;
}

static int data_queue_free(struct DataQueue *queue){
    kfree(queue->array_pt);
    printk(KERN_INFO "Queue pointer freed\n");
    return 0;
}

static int data_push(struct DataQueue *queue, struct Data data) {
    if (queue->data_count == queue_size) {
        return -1;
    }
    queue->array_pt[(queue->first_pos + queue->data_count) % queue_size] = data;
    queue->data_count += 1;
    return 0;
}

static struct Data data_pop(struct DataQueue *queue) { // problem with return type
    int tmp;
    if (queue->data_count == 0) {
        //returns data of length -1 on failure;
        struct Data err_data; //this is a local variable
        err_data.length = -1;
        return err_data;
    }
    queue->data_count -= 1;
    tmp = queue->first_pos;
    queue->first_pos = (queue->first_pos + 1) % queue_size;
    return queue->array_pt[tmp];
}

//this is for debugging
static void data_print(struct DataQueue *queue) {
    for (int i = 0; i < queue->data_count; i++){
        printk("Data %d: %s\n", i, queue->array_pt[(queue->first_pos + i) % queue_size]);
    }
}

//--------------------Variables---------------------------------

static dev_t dev = 0;
static int registered_process = -1; //TODO rename this
struct task_struct *task; //this too maybe?
struct task_struct *comm_thread;
struct gpio_dev g_dev;
struct DataQueue queue_to_send;
//struct DataQueue queue_received;

static int gpio_pin_number = -1;
//enables indicating the pin number during initialization
//S_IRUGO means the parameter can be read but cannot be changed
module_param(gpio_pin_number, int, S_IRUGO);

static int comm_role = 0;
//master == 0, slave == 1;
module_param(comm_role, int, S_IRUGO);

//cleanup helper variables, useful for error handling
static int chrdev_allocated = 0;
static int device_registered = 0; //TODO rename these
static int gpio_requested = 0;
static int kthread_started = 0;
static int queue_kmalloc = 0;

//--------------------Auxiliary Functions------------------------

//self explanatory
static void cleanup_func(void){
    if(chrdev_allocated) {
        unregister_chrdev_region(dev,1);
    }
    if(device_registered) {
        cdev_del(&(g_dev.cdev));
    }
    if(gpio_requested) {
        gpio_free(gpio_pin_number);
    }
    if(kthread_started) {
        kthread_stop(comm_thread);
    }
    if(queue_kmalloc) {
        data_queue_free(&queue_to_send);
    }
}

static int gpio_setup_cdev(struct gpio_dev *g_dev){
    cdev_init(&g_dev->cdev, &gpio_fops);
    g_dev->cdev.owner = THIS_MODULE;
    g_dev->cdev.ops = &gpio_fops;
    if(cdev_add(&g_dev->cdev, dev, 1) < 0){
        return -1;
    }
    return 0;
}

static void signal_to_pid_datarecv(void){ // change type maybe
    if (registered_process > 0){
        if(send_sig_info(SIGDATARECV, (struct kernel_siginfo*) 1, task) < 0) {
            printk(KERN_WARNING "Error sending data receive signal\n");
        }
    }
}

static int reset(void) {
    gpio_direction_output(gpio_pin_number, 0);
    mdelay(1000);
    gpio_direction_input(gpio_pin_number);
    //printk("Reset func triggered!\n");
    return 0;
}

/*
static void send_byte_master(void) {

}

static void read_byte_master(void) {

}
*/

static int master_mode(void *p) {
    int read_mode = 0;
    printk("Kernel thread working!\n");
    //reset returns -1 if no presence, 0 if no msg from slave, 1 if 
    // there is a message from the slave
    
    while(!kthread_should_stop()) {
        int status = reset();
        if(status == -1) {
            printk("Slave is not present\n");
        }
        else if (status == 0) {
            //if there is message in queue, send it
        }
        else {
            read_mode = 1; // maybe I don't even need this variable
        }
        //maybe wait a while after each reset
    }
    return 0;
}

static int slave_mode(void *p) {
    //busy wait until gpio pin lights up?
    return 0;
}


//----------------File Operation Functions------------------------

static int gpio_open(struct inode *inode, struct file *file){
    printk("Device file opened\n");
    return 0;
}

static int gpio_close(struct inode *inode, struct file *file){
    printk("Device file closed\n");
    return 0;
}

static ssize_t gpio_read(struct file *filp, char __user *buff, size_t count, loff_t *offp){
    printk(KERN_INFO "Device read, sending signal");
    signal_to_pid_datarecv();
    data_print(&queue_to_send);
    /*
    uint8_t gpio_state = gpio_get_value(gpio_pin_number);
    
    count = 1;
    if(copy_to_user(buff, &gpio_state, count) > 0) {
        printk(KERN_WARNING "ERROR");
    }

    printk(KERN_INFO "gpio_pin_number state = %d \n", gpio_state);
    */
    return 0;
}
static ssize_t gpio_write(struct file *filp, const char __user *buff, size_t count, loff_t *offp){
    char msg[10];
    if(count > 10) {
        printk("Data too big\n");
        return -1;
    }
    if(copy_from_user(msg, buff, count) > 0) {
        printk(KERN_WARNING "ERROR1");
    }

    //printk("Data: %s, size: %d\n", msg, count);
    struct Data tmp;
    for (int i = 0; i < count, i++){
        tmp.buffer[i] = msg[i];
    }
    tmp.length = count;
    data_push(&queue_to_send, tmp);

    return count;
}

static long gpioctl(struct file *filp, unsigned int cmd, unsigned long arg){
    if(cmd == USER_APP_REG) {
        if(copy_from_user(&registered_process, (int*) arg, 4) > 0) {
            printk(KERN_WARNING "Error reading pid");
            return -1;
        }
        else{
            task = pid_task(find_get_pid(registered_process), PIDTYPE_PID);
            printk(KERN_INFO "Registered pid: %d\n", registered_process);
            return 0;
        }
    }
    if(cmd == START_COMMS) {
        /*
        if(comm_role == 0) {
            master_mode();
        }
        else if (comm role == 1) {
            slave_mode();
        }
        */
        reset();
    }
    return 0;
}

//-----------------Initializer----------------------------------

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

    if(gpio_is_valid(gpio_pin_number) == false){
        printk(KERN_WARNING "Invalid GPIO\n");
        cleanup_func();
        return -1;
    }

    gpio_requested = 1;
    if(gpio_request(gpio_pin_number, "gpio") < 0) { //this may cause a problem
        printk(KERN_WARNING "GPIO request error\n");
        cleanup_func();
        return -1;
    }
    gpio_direction_input(gpio_pin_number);
    gpio_export(gpio_pin_number, false); //what does this do?

    
    if(data_queue_init(&queue_to_send) < 0){
        printk(KERN_WARNING "kmalloc failed\n");
        cleanup_func();
        return -1;
    }
    queue_kmalloc = 1;

    kthread_started = 1;
    if(comm_role == 0) {
        comm_thread = kthread_run(master_mode, NULL, "master_thread");
        //failure case?
    }
    else if(comm_role == 1) {
        comm_thread = kthread_run(slave_mode, NULL, "slave_thread");
    }

    printk("Driver loaded\n");

    return 0;
}

//---------------Safe Remove Func-------------------------------
static void __exit gpio_driver_exit(void){
    cleanup_func();
    printk("Driver removed\n");
}

//---------------Main (kind of)---------------------------------------------
module_init(gpio_driver_init);
//put functions here
module_exit(gpio_driver_exit);
