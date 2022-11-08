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
#include <linux/mutex.h>
MODULE_LICENSE("GPL");

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
    //printk(KERN_INFO "Queue pointer freed\n");
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

//this function copies first element and removes it from the queue
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

//this function just copies the first element
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

//this is for debugging
static void data_print(struct DataQueue *queue) {
    int i;
    printk("Printing data\n");
    for (i = 0; i < queue->data_count; i += 1){
        printk("Data %d: %s", i, (queue->array_pt[(queue->first_pos + i) % queue_size]).buffer);
    }
}

//--------------------Variables---------------------------------

static dev_t dev = 0;
static int registered_process = -1;
struct task_struct *task;
struct task_struct *comm_thread;
struct gpio_dev g_dev;
struct DataQueue queue_to_send;
struct Data received_data;
static int prev_data_not_read = 0;
struct mutex mtx1;

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
static int gpio_exported = 0;
static int kthread_started = 0;
static int queue_kmalloc = 0;

//--------------------Auxiliary Functions------------------------

//self explanatory
static void cleanup_func(void){
    if(queue_kmalloc) {
        data_queue_free(&queue_to_send);
    }
    if(kthread_started) {
        kthread_stop(comm_thread);
    }
    if(gpio_exported) {
        gpio_unexport(gpio_pin_number);
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
    if(queue_to_send.data_count > 0) {
        udelay(300);
        gpio_direction_input(gpio_pin_number);
        udelay(200);
    }
    else{
        udelay(500);
        gpio_direction_input(gpio_pin_number);
    }
    udelay(100);
    if(gpio_get_value(gpio_pin_number) == 0){
        //slave present, check if it has data to send
        udelay(150); //correct timings
        if(gpio_get_value(gpio_pin_number) == 0) {
            udelay(150);
            return 1;
        }
        udelay(150);
        return 0;
    }
    udelay(300);
    return -1;
}

static void send_response(void){

}

static char read_byte(void){
    char byte = 0x00;
    int b[8];
    int i;
    for(i = 0; i < 8; i += 1){
        udelay(100);
        b[i] = gpio_get_value(gpio_pin_number); //timings
        udelay(100);
    }

    for(i = 0; i < 8; i += 1){
        byte = byte | (b[i] << i);
    }
    udelay(750);
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
        //printk("%c\n",message[i]);
    }

    if(message[0] != 0xAA) {
        printk("Header wrong\n");
        is_corrupted = 1;
    }

    msg_length = (int) message[1];

    if((msg_length < 0) || (msg_length > 10)) {
        printk("Length wrong\n");
        is_corrupted = 1;
    }
    else {
        checksum = message[0];
        for (i = 1; i < msg_length + 2; i += 1) {
            checksum = checksum ^ message[i];
        }
        if(checksum != message[2 + msg_length]) {
            printk("Checksum wrong\n");
            is_corrupted = 1;
        }
    }

    if(is_corrupted) {
        //send 0x00
        
    }
    else {
        mutex_lock(&mtx1);
        received_data.length = msg_length;
        for (i = 0; i < msg_length; i += 1) {
            received_data.buffer[i] = message[2 + i];
        }
        prev_data_not_read = 1;
        mutex_unlock(&mtx1);
        //send 0x0F
        signal_to_pid_datarecv();
    }
    
    //between each bit start a timeout (or transmission ended) timer
    //when transmission ends check data integrity (length and checksum)
    //if correct, check if command
    //if command respond
    //if no command send ack
    //if corruption, send fail, listen again
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
            udelay(150);
            gpio_direction_input(gpio_pin_number);
            udelay(50);
        }
        else{
            gpio_direction_output(gpio_pin_number, 0);
            udelay(30);
            gpio_direction_input(gpio_pin_number);
            udelay(170);
        }   
    }
    udelay(750);
}

static void send_message(void) {
    struct Data dt;
    int i;
    char checksum;
    char ack;
    data_read_top(&queue_to_send, &dt); //this doesn't fail unless the queue is empty (we always check before calling send_message())
                
    //calculate checksum
    checksum = 0xAA ^ ((char) dt.length);
    for (i = 0; i < dt.length; i += 1) {
        checksum = checksum ^ dt.buffer[i];
    }
    send_byte((char) 0xAA);
    send_byte((char) dt.length);
    for (i = 0; i < dt.length; i += 1) {
        send_byte(dt.buffer[i]);
    }
    send_byte(checksum);
    mdelay(10);
    //------before this-------
    //if command get response, send ack
    //else read ack, if ok, data_pop
    //if ack_fail, reset
    //if no response after udelay, reset
    //-------------------------
    //read ack, if fail, reset
    //else data_pop
}

static int master_mode(void *p) {
    printk("Kernel thread for master started!\n");
    while((registered_process == -1) && (!kthread_should_stop())) {
        //User app is not working, happy busy waiting!
        mdelay(1000);
    }

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
            printk("Slave is not present\n");
        }
        else if (status == 0) {
            //if there is message in queue, send it
            printk("Slave has no message\n");
            if (queue_to_send.data_count > 0) {
                send_message();
            }
        }
        else {
            printk("Slave has a message\n");
            read_message();
        }
        mdelay(1000);
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
        send_mode = (queue_to_send.data_count > 0);
        while(gpio_get_value(gpio_pin_number) == 1 && (!kthread_should_stop())){
            //wait until there is a reset signal
            //busy waits, implement irq?
        }
        udelay(200);
        if(gpio_get_value(gpio_pin_number) == 1){
            continue;
        }
        udelay(200);
        if(gpio_get_value(gpio_pin_number) == 1){
            read_mode = 1;
            printk("Master have message to send\n");
        }
        udelay(150);
        gpio_direction_output(gpio_pin_number, 0);
        udelay(100);
        gpio_direction_input(gpio_pin_number);
        if(send_mode) {
            udelay(50);
            gpio_direction_output(gpio_pin_number, 0);
            udelay(100);
            gpio_direction_input(gpio_pin_number);
            udelay(100);
            send_message();
        }
        else if(read_mode){
            udelay(250);
            read_message();
        }
        else {
            udelay(250);
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

static ssize_t gpio_read(struct file *filp, char __user *buff, size_t count, loff_t *offp){
    mutex_lock(&mtx1);
    if(copy_to_user(buff, received_data.buffer, received_data.length) > 0){
        return -1;
    }
    prev_data_not_read = 0;
    mutex_unlock(&mtx1);
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
    printk("%d\n", queue_to_send.data_count);
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
            queue_to_send.data_count = 0;
            queue_to_send.first_pos = 0;
            printk(KERN_INFO "User app unregistered\n");
        }
    }
    return 0;
}

//-----------------Initializer----------------------------------

static int __init gpio_driver_init(void){
    char* name;
    mutex_init(&mtx1);
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

    gpio_exported = 1;
    gpio_export(gpio_pin_number, false); //not sure if this is relevant

    
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
