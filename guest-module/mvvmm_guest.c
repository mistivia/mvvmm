#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/reboot.h>
#include <linux/workqueue.h>
#include <linux/ioport.h>
#include <linux/pm.h>
#include <linux/limits.h>
#include <linux/reboot.h>

#define IRQ 5
#define PORT 0x300
#define DRIVER_NAME "mvvmm_guest"

#define CMD_HALT     1

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mistivia");
MODULE_DESCRIPTION("MVVMM Guest Power Control");
MODULE_VERSION("2.0");

static int pending_command = 0;
static struct work_struct power_work;

static void mvvmm_poweroff(void)
{
    outb(CMD_HALT, PORT);
    while (1) ;
}


static void power_work_func(struct work_struct *work)
{
    switch (pending_command) {
case CMD_HALT:
        printk(KERN_INFO "MVVMM: IRQ Halt received. Scheduling orderly poweroff.\n");
        orderly_poweroff(true);
        break;
        
    default:
        printk(KERN_WARNING "MVVMM: Unknown pending command %d\n", pending_command);
    }
}

static irqreturn_t my_irq_handler(int irq, void *dev_id)
{
    unsigned char val;
    val = inb(PORT);
    if (val == CMD_HALT) {
        pending_command = val;
        schedule_work(&power_work);
        return IRQ_HANDLED;
    }

    return IRQ_NONE;
}

static int __init mvvmm_init(void)
{
    int ret;

    printk(KERN_INFO "MVVMM: Loading Guest Module...\n");

    if (!request_region(PORT, 1, DRIVER_NAME)) {
        printk(KERN_ERR "MVVMM: Failed to request region 0x%x\n", PORT);
        return -EBUSY;
    }
    if (pm_power_off) {
        printk(KERN_WARNING "Overriding existing power_off handler\n");
    }
    pm_power_off = mvvmm_poweroff;

    INIT_WORK(&power_work, power_work_func);

    ret = request_irq(IRQ, my_irq_handler, IRQF_SHARED, DRIVER_NAME, (void *)(my_irq_handler));
    if (ret) {
        printk(KERN_ERR "MVVMM: Failed to request IRQ %d\n", IRQ);
        release_region(PORT, 1);
        return ret;
    }

    printk(KERN_INFO "MVVMM: Module loaded. Listening on IRQ %d, Port 0x%x\n", IRQ, PORT);
    return 0;
}

static void __exit mvvmm_exit(void)
{
    if (pm_power_off == mvvmm_poweroff) {
        pm_power_off = NULL;
    }
    free_irq(IRQ, (void *)(my_irq_handler));
    cancel_work_sync(&power_work);
    release_region(PORT, 1);
    printk(KERN_INFO "MVVMM: Module unloaded.\n");
}

module_init(mvvmm_init);
module_exit(mvvmm_exit);