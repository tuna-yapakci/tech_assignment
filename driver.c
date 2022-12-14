#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/gpio.h>
#include <asm/uaccess.h>
#include <linux/ioctl.h>
#include <linux/pid.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
MODULE_LICENSE("GPL");
MODULE_AUTHOR("tuna-yapakci");

#define MAGIC 'k'
#define USER_APP_REG _IOW(MAGIC, 1, int*)
#define USER_APP_UNREG _IO(MAGIC, 2)
#define SIGDATARECV 47
#define MASTERNAME "gpio_master"
#define SLAVENAME "gpio_slave"

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
    uint8_t length;
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

static int data_add_front(struct DataQueue *queue, struct Data data) {
    int new_pos;
    if (queue->data_count == queue_size) {
        return -1;
    }
    new_pos = (queue->first_pos) - 1;
    if (new_pos == -1) {
        new_pos = queue_size - 1;
    }
    queue->first_pos = new_pos;
    queue->array_pt[queue->first_pos] = data;
    queue->data_count += 1;
    return 0;
}

//This function copies first element and removes it from the queue
static int data_pop(struct DataQueue *queue, struct Data *data_to_copy) {
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

//This function just copies the first element
static int data_read_top(struct DataQueue *queue, struct Data *data_to_copy) {
    int i;
    if (queue->data_count == 0) {
        return -1;
    }
    data_to_copy->length = (queue->array_pt[queue->first_pos]).length;
    for (i = 0; i < data_to_copy->length; i += 1){
        data_to_copy->buffer[i] = queue->array_pt[queue->first_pos].buffer[i];
    }
    return 0;
}

/* This function was for debugging
static void data_print(struct DataQueue *queue) {
    int i;
    printk("Printing data\n");
    for (i = 0; i < queue->data_count; i += 1){
        printk("Data %d: %s", i, (queue->array_pt[(queue->first_pos + i) % queue_size]).buffer);
    }
}
*/
//--------------------Variables---------------------------------

//Stuff needed to initialize a character device
static dev_t dev = 0;
struct gpio_dev g_dev;

//task_struct for the kernel thread that gets created when a process registers
struct task_struct *comm_thread;

//Queue to store messages that are going to get sent
struct DataQueue queue_to_send;

//A variable that holds the PID of current registered process (-1 means no process is registered)
static int registered_process = -1;
// and a task_struct related to that process
struct task_struct *task;

//Following variables hold last read message and make sure it is read before another one arrives
struct Data received_data;
static int prev_data_not_read = 0;

//Mutexes to guard shared memory
struct mutex mtx1;
struct mutex mtx2;

//Stores a kernel timestamp, needed for accuracy of timing during communication
static u64 timer;

//Enables indicating the pin number during initialization
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

//Self explanatory, gets called when unloading module, or failure during initialization
static void cleanup_func(void){
    if(queue_kmalloc) {
        data_queue_free(&queue_to_send);
    }
    if(kthread_started) {
        kthread_stop(comm_thread);
    }
    if(gpio_requested) {
        gpio_free(gpio_pin_number);
    }
    if(device_registered) {
        cdev_del(&(g_dev.cdev));
    }
    if(chrdev_allocated) {
        unregister_chrdev_region(dev,1);
    }
    
}

//Sets up and adds a character device to the system
static int gpio_setup_cdev(struct gpio_dev *g_dev){
    cdev_init(&g_dev->cdev, &gpio_fops);
    g_dev->cdev.owner = THIS_MODULE;
    g_dev->cdev.ops = &gpio_fops;
    if(cdev_add(&g_dev->cdev, dev, 1) < 0){
        return -1;
    }
    return 0;
}

//Sends data received signal to the registered process (user app)
static void signal_to_pid_datarecv(void){
    if (registered_process > 0){
        if(send_sig_info(SIGDATARECV, (struct kernel_siginfo*) 1, task) < 0) {
            printk(KERN_WARNING "Error sending data receive signal\n");
        }
    }
}


//Functions that implement our communication protocol
//(more info on the report)
static int reset(void);
static char read_byte(void);
static void read_message(void);
static void send_message(void);
static void send_byte(char byte);

static int reset(void) {
    //reset returns -1 if no presence, 0 if no msg from slave, 1 if 
    // there is a message from the slave, 2 if slave has no msg but master has
    int slave_present;
    int slave_message;
    int master_message;
    mutex_lock(&mtx2);
    master_message = (queue_to_send.data_count > 0);
    mutex_unlock(&mtx2);
    gpio_direction_output(gpio_pin_number, 0);
    timer = ktime_get_ns();
    if(master_message) {
        //udelay(300);
        timer += 300000;
        while(timer > ktime_get_ns()) {}
        gpio_direction_input(gpio_pin_number);
        //udelay(200);
        timer += 200000;
        while(timer > ktime_get_ns()) {}
    }
    else{
        //udelay(500);
        timer += 500000;
        while(timer > ktime_get_ns()) {}
        gpio_direction_input(gpio_pin_number);
    }
    //udelay(100);
    timer += 100000;
    while(timer > ktime_get_ns()) {}
    slave_present = (gpio_get_value(gpio_pin_number) == 0);
    //udelay(150);
    timer += 150000;
    while(timer > ktime_get_ns()) {}
    slave_message = (gpio_get_value(gpio_pin_number) == 0);
    //udelay(150);
    timer += 150000;
    while(timer > ktime_get_ns()) {}
    if(!slave_present){
        return -1;
    }
    else if(slave_message) {
        return 1;
    }
    else if (master_message){
        return 2;
    }
    else{
        return 0;
    }
}

static char read_byte(void){
    char byte = 0x00;
    int b[8];
    int i;
    timer = ktime_get_ns();
    for(i = 0; i < 8; i += 1){
        //udelay(40);
        timer += 40000;
        while(timer > ktime_get_ns()) {}
        b[i] = gpio_get_value(gpio_pin_number);
        //udelay(60);
        timer += 60000;
        while(timer > ktime_get_ns()) {}
    }

    for(i = 0; i < 8; i += 1){
        byte = byte | (b[i] << i);
    }
    //udelay(750);
    timer += 750000;
    while(timer > ktime_get_ns()) {}
    return byte;
}

static void read_message(void){
    char message[13];
    int i;
    int msg_length;
    char checksum;
    int is_corrupted = 0;

    for (i = 0; i < 13; i += 1){
        message[i] = read_byte();
    }

    if(message[0] != 0xAA) {
        is_corrupted = 1;
    }

    msg_length = (int) message[1];

    if((msg_length < 0) || (msg_length > 10)) {
        is_corrupted = 1;
    }
    else {
        checksum = message[0];
        for (i = 1; i < msg_length + 2; i += 1) {
            checksum = checksum ^ message[i];
        }
        if(checksum != message[2 + msg_length]) {
            is_corrupted = 1;
        }
    }

    if(is_corrupted) {
        mdelay(15);
        send_byte(0x00);
        
    }
    else {
        mutex_lock(&mtx1);
        received_data.length = msg_length;
        for (i = 0; i < msg_length; i += 1) {
            received_data.buffer[i] = message[2 + i];
        }
        prev_data_not_read = 1;
        mutex_unlock(&mtx1);
        mdelay(15);
        send_byte(0x0F);

        signal_to_pid_datarecv();
    }
}

static void send_byte(char byte) {
    int i;
    int b[8];
    timer = ktime_get_ns();
    for(i = 0; i < 8; i += 1) {
        b[i] = (int) ((byte >> i) & (0x01));
    }
    for(i = 0; i < 8; i += 1) {
        if (b[i] == 0)  {
            gpio_direction_output(gpio_pin_number, 0);
            //udelay(65);
            timer += 65000;
            while(timer > ktime_get_ns()) {}
            gpio_direction_input(gpio_pin_number);
            //udelay(35);
            timer += 35000;
            while(timer > ktime_get_ns()) {}
        }
        else{
            gpio_direction_output(gpio_pin_number, 0);
            //udelay(15);
            timer += 15000;
            while(timer > ktime_get_ns()) {}
            gpio_direction_input(gpio_pin_number);
            //udelay(85);
            timer += 85000;
            while(timer > ktime_get_ns()) {}
        }   
    }
    //udelay(750);
    timer += 750000;
    while(timer > ktime_get_ns()) {}
}

static void send_message(void) {
    struct Data dt;
    int i;
    char checksum;
    char ack;
    int rest;

    mutex_lock(&mtx2);
    data_read_top(&queue_to_send, &dt); //this doesn't fail unless the queue is empty (we always check before calling send_message())
    mutex_unlock(&mtx2);

    rest = 10 - (dt.length);
    checksum = 0xAA ^ (dt.length);
    for (i = 0; i < dt.length; i += 1) {
        checksum = checksum ^ dt.buffer[i];
    }
    send_byte((char) 0xAA);
    send_byte((char) dt.length);
    for (i = 0; i < dt.length; i += 1) {
        send_byte(dt.buffer[i]);
    }
    send_byte(checksum);
    for (i = 0; i < rest; i += 1) {
        send_byte((char) 0xFF);
    }
    mdelay(10);
    while((gpio_get_value(gpio_pin_number) == 1)  && (!kthread_should_stop())) {
            //busy wait
    }
    ack = read_byte();
    if(ack == 0x0F){
        mutex_lock(&mtx2);
        data_pop(&queue_to_send, &dt);
        mutex_unlock(&mtx2);
    }

}

static int master_mode(void *p) {
    printk("Kernel thread for master started!\n");

    while(!kthread_should_stop()) {
        int status;

        mutex_lock(&mtx1);
        if(prev_data_not_read) {
            mutex_unlock(&mtx1);
            continue;
        }
        mutex_unlock(&mtx1);

        status = reset();
        if(status == -1) {
            //printk("Master: Slave is not present\n");
        }
        else if (status == 0) {
            //printk("Master: Nobody has a message\n");
        }
        else if (status == 1) {
            //printk("Master: Slave has a message\n");
            read_message();
        }
        else {
            //printk("Master: Master has a message");
            send_message();
        }
        mdelay(10);
    }
    return 0;
}

static int slave_mode(void *p) {
    printk("Kernel thread for slave started!\n");

    while(!kthread_should_stop()) {
        int send_mode;
        int read_mode = 0;
        
        mutex_lock(&mtx1);
        if(prev_data_not_read) {
            mutex_unlock(&mtx1);
            continue;
        }
        mutex_unlock(&mtx1);

        mutex_lock(&mtx2);
        send_mode = (queue_to_send.data_count > 0);
        mutex_unlock(&mtx2);

        while((gpio_get_value(gpio_pin_number) == 1) && (!kthread_should_stop())) {
            //busy wait
        }

        timer = ktime_get_ns();
        //udelay(350);
        timer += 350000;
        while(timer > ktime_get_ns()) {}
        read_mode = (gpio_get_value(gpio_pin_number) == 1);
        //udelay(200);
        timer += 200000;
        while(timer > ktime_get_ns()) {}
        gpio_direction_output(gpio_pin_number, 0);
        //udelay(100);
        timer += 100000;
        while(timer > ktime_get_ns()) {}
        gpio_direction_input(gpio_pin_number);
        if(send_mode) {
            //printk("Slave: Sending message");
            //udelay(50);
            timer += 50000;
            while(timer > ktime_get_ns()) {}
            gpio_direction_output(gpio_pin_number, 0);
            //udelay(100);
            timer += 100000;
            while(timer > ktime_get_ns()) {}
            gpio_direction_input(gpio_pin_number);
            //udelay(100);
            timer += 100000;
            while(timer > ktime_get_ns()) {}
            send_message();
        }
        else if(read_mode){
            //printk("Slave: Reading message");
            //udelay(250);
            timer += 250000;
            while(timer > ktime_get_ns()) {}
            read_message();
        }
        else {
            //udelay(250);
            timer += 250000;
            while(timer > ktime_get_ns()) {}
        }
    }
    return 0;
}


//----------------File Operation Functions------------------------

//There is nothing specific we do when the dev file gets opened or closed
static int gpio_open(struct inode *inode, struct file *file){
    //printk("Device file opened\n");
    return 0;
}

static int gpio_close(struct inode *inode, struct file *file){
    //printk("Device file closed\n");
    return 0;
}

//Sends the received data to the user space (when dev file is read)
static ssize_t gpio_read(struct file *filp, char __user *buff, size_t count, loff_t *offp){
    uint8_t len;
    mutex_lock(&mtx1);
    if (prev_data_not_read == 0) {
        mutex_unlock(&mtx1);
        return -1;
    }
    len = received_data.length;
    if(copy_to_user(buff, &len, 1) > 0){
        mutex_unlock(&mtx1);
        return -1;
    }
    if(copy_to_user(buff + 1, received_data.buffer, received_data.length) > 0){
        mutex_unlock(&mtx1);
        return -1;
    }
    prev_data_not_read = 0;
    mutex_unlock(&mtx1);
    return 0;
}

//Adds data written to dev file to the queue
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
        return -1;
    }

    for (i = 0; i < count; i += 1){
        tmp.buffer[i] = msg[i];
    }
    tmp.length = count;
    //Send responses first by adding them on the front 
    if (tmp.buffer[0] == 0xBC) {
        mutex_lock(&mtx2);
        if (data_add_front(&queue_to_send, tmp) < 0) {
            mutex_unlock(&mtx2);
            printk(KERN_WARNING "Queue is full, write failed\n");
            return -1;
        };
        mutex_unlock(&mtx2);
    }
    else {
        mutex_lock(&mtx2);
        if (data_push(&queue_to_send, tmp) < 0) {
            mutex_unlock(&mtx2);
            printk(KERN_WARNING "Queue is full, write failed\n");
            return -1;
        };
        mutex_unlock(&mtx2);
    }
    return count;
}

//Does things that are necessary to register and unregister user level processes
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
            mutex_lock(&mtx2);
            queue_to_send.data_count = 0;
            queue_to_send.first_pos = 0;
            mutex_unlock(&mtx2);
            printk(KERN_INFO "User app unregistered\n");
        }
    }
    return 0;
}

//-----------------Initializer----------------------------------

//This function is called to load the driver into the kernel (by insmod)
static int __init gpio_driver_init(void){
    char* name;
    mutex_init(&mtx1);
    mutex_init(&mtx2);
    if(comm_role == 0) {
        name = MASTERNAME;
    }
    else if (comm_role == 1) {
        name = SLAVENAME;
    }
    printk("%s pin is %d\n", name, gpio_pin_number);
    
    chrdev_allocated = 1;
    if(alloc_chrdev_region(&dev, 0, 1, name) < 0) {
        printk(KERN_WARNING "Error during dev number allocation\n");
        cleanup_func();
        return -1;
    }

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
    if(gpio_request(gpio_pin_number, name) < 0) {
        printk(KERN_WARNING "GPIO request error\n");
        cleanup_func();
        return -1;
    }
    gpio_direction_input(gpio_pin_number);

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
//This function is called when removing the module from the kernel (by rmmod)
static void __exit gpio_driver_exit(void){
    cleanup_func();
    printk("Driver removed\n");
}

//---Kernel stuff for loading and unloading module--------------
module_init(gpio_driver_init);
module_exit(gpio_driver_exit);
