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
#define USER_APP_UNREG _IO(MAGIC, 2)
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

static int data_pop(struct DataQueue *queue, struct Data *data_to_copy) { // problem with return type
    int i;
    if (queue->data_count == 0) {
        return -1;
    }
    queue->data_count -= 1;
    data_to_copy->length = (queue->array_pt[queue->first_pos]).length;
    for (i = 0; i < data_to_copy->length; i += 1){
        data_to_copy->buffer[i] = queue->array_pt[queue->first_pos].buffer[i];
    }
    queue->first_pos = (queue->first_pos + 1) % queue_size;
    return 0;
}

//this is for debugging
static void data_print(struct DataQueue *queue) {
    int i;
    for (i = 0; i < queue->data_count; i += 1){
        printk("Data %d: %s", i, (queue->array_pt[(queue->first_pos + i) % queue_size]).buffer);
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

//enables indicating the pin number during initialization
//S_IRUGO means the parameter can be read but cannot be changed
static int gpio_pin_number = -1;
module_param(gpio_pin_number, int, S_IRUGO);

//master == 0, slave == 1;
static int comm_role = 0;
module_param(comm_role, int, S_IRUGO);

//cleanup helper variables, useful for error handling
static int chrdev_allocated = 0;
static int device_registered = 0;
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
    //reset returns -1 if no presence, 0 if no msg from slave, 1 if 
    // there is a message from the slave
    gpio_direction_output(gpio_pin_number, 0);
    mdelay(1000);
    gpio_direction_input(gpio_pin_number);
    return 0;
}

static void read_byte(void){

}

static void read_message(void){
    read_byte();
}

static void send_byte(char byte) {
    int i;
    int b[8];
    for(i = 0; i < 8; i += 1) {
        b[i] = (int) ((byte >> i) & (0x01)); //check if this is correct
    }
    for(i = 0; i < 8; i += 1) {
        if (b[i] == 0)  {
            gpio_direction_output(gpio_pin_number, 0);
            udelay(60);
            gpio_direction_input(gpio_pin_number);
            udelay(10);
        }
        else{
            gpio_direction_output(gpio_pin_number, 0);
            udelay(15);
            gpio_direction_input(gpio_pin_number);
            udelay(55);
        }   
    }
}

static int master_mode(void *p) {
    printk("Kernel thread working!\n");
    while((registered_process == -1) && (!kthread_should_stop())) {
        //User app is not working, happy busy waiting!
        mdelay(1000);
    }

    while(!kthread_should_stop()) {
        int status = reset();
        if(status == -1) {
            printk("Slave is not present\n");
        }
        else if (status == 0) {
            //if there is message in queue, send it
            if (queue_to_send.data_count > 0) {
                struct Data dt;
                int i;
                data_pop(&queue_to_send, &dt); //this doesn't fail unless the queue is empty
                
                //calculate checksum
                //send header
                send_byte((char) 0xAA);
                //send message length
                for (i = 0; i < dt.length; i += 1) {
                    //send byte from message
                }
                //send checksum

                //wait response
                //send ack
            }
        }
        else {
            read_message();
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
    struct Data tmp;
    int i;

    if(count > 10) {
        printk("Data too big\n");
        return -1;
    }
    if(copy_from_user(msg, buff, count) > 0) {
        printk(KERN_WARNING "Error writing data");
    }

    for (i = 0; i < count; i += 1){
        tmp.buffer[i] = msg[i];
    }
    tmp.length = count;
    if (data_push(&queue_to_send, tmp) < 0) {
        printk(KERN_WARNING "Queue is full, write failed\n");
        return -1;
    };

    return count;
}

static long gpioctl(struct file *filp, unsigned int cmd, unsigned long arg){
    if(cmd == USER_APP_REG) {
        if (registered_process >= 0) {
            printk(KERN_WARNING "User app already registered\n");
            return -1;
        }
        if(copy_from_user(&registered_process, (int*) arg, 4) > 0) {
            printk(KERN_WARNING "Error reading pid\n");
            return -1;
        }
        else{
            task = pid_task(find_get_pid(registered_process), PIDTYPE_PID);
            printk(KERN_INFO "Registered pid: %d\n", registered_process);

            kthread_started = 1;
            if(comm_role == 0) {
                comm_thread = kthread_run(master_mode, NULL, "master_thread");
                //failure case?
            }
            else if(comm_role == 1) {
                comm_thread = kthread_run(slave_mode, NULL, "slave_thread");
            }
            return 0;
        }
    }
    if(cmd == USER_APP_UNREG) {
        if (registered_process < 0) {
            printk(KERN_WARNING "No app is registered\n");
        }
        else {
            registered_process = -1;
            kthread_stop(comm_thread);
            kthread_started = 0;
        }
    }
    return 0;
}

//-----------------Initializer----------------------------------

static int __init gpio_driver_init(void){
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
    if(gpio_request(gpio_pin_number, "gpio") < 0) { //the label should be different for master and slave (when running on same computer?)
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

    printk("Driver loaded\n");

    return 0;
}

//---------------Safe Remove Func-------------------------------
static void __exit gpio_driver_exit(void){
    cleanup_func();
    printk("Driver removed\n");
}

//---Kernel stuff for loading and unloading module--------------
module_init(gpio_driver_init);
module_exit(gpio_driver_exit);
