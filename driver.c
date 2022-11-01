#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
MODULE_LICENSE("Dual BSD/GPL");

static int hello_init(void){
    printk(KERN_ALERT "Hello,world!\n");
    return 0;
}

static void hello_exit(void){
    printk(KERN_ALERT "Goodbye, cruel world\n");
}

module_init(hello_init);
module_exit(hello_exit);

//token : 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