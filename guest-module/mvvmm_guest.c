#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/reboot.h>
#include <linux/workqueue.h>
#include <linux/ioport.h>
#include <linux/limits.h>
#include <linux/reboot.h>

#define IRQ 5
#define PORT 0x300
#define DRIVER_NAME "mvvmm_guest"

#define CMD_HALT     1
#define CMD_REBOOT   2

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mistivia");
MODULE_DESCRIPTION("MVVMM Guest Power Control");
MODULE_VERSION("2.0");

static int pending_command = 0;
static struct work_struct power_work;

static int mvvmm_reboot_notify(struct notifier_block *nb, unsigned long action, void *data)
{
    switch (action) {
    case SYS_RESTART:
        printk(KERN_EMERG "MVVMM: Sending REBOOT signal (0x%x <- %d)\n", PORT, CMD_REBOOT);
        outb(CMD_REBOOT, PORT);
        break;

    case SYS_HALT:
    case SYS_POWER_OFF:
        printk(KERN_EMERG "MVVMM: Sending HALT signal (0x%x <- %d)\n", PORT, CMD_HALT);
        outb(CMD_HALT, PORT);
        break;
    }
    return NOTIFY_DONE;
}

static struct notifier_block mvvmm_notifier = {
    .notifier_call = mvvmm_reboot_notify,
    .priority = INT_MIN, 
};

static void power_work_func(struct work_struct *work)
{
    switch (pending_command) {
    case CMD_HALT:
        printk(KERN_INFO "MVVMM: IRQ Halt received. Scheduling orderly poweroff.\n");
        orderly_poweroff(true);
        break;

    case CMD_REBOOT:
        printk(KERN_INFO "MVVMM: IRQ Reboot received. Scheduling orderly reboot.\n");
        orderly_reboot();
        break;
        
    default:
        printk(KERN_WARNING "MVVMM: Unknown pending command %d\n", pending_command);
    }
}

static irqreturn_t my_irq_handler(int irq, void *dev_id)
{
    unsigned char val;
    val = inb(PORT);
    if (val == CMD_HALT || val == CMD_REBOOT) {
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

    ret = register_reboot_notifier(&mvvmm_notifier);
    if (ret) {
        printk(KERN_ERR "MVVMM: Failed to register reboot notifier\n");
        release_region(PORT, 1);
        return ret;
    }

    INIT_WORK(&power_work, power_work_func);

    ret = request_irq(IRQ, my_irq_handler, IRQF_SHARED, DRIVER_NAME, (void *)(my_irq_handler));
    if (ret) {
        printk(KERN_ERR "MVVMM: Failed to request IRQ %d\n", IRQ);
        unregister_reboot_notifier(&mvvmm_notifier);
        release_region(PORT, 1);
        return ret;
    }

    printk(KERN_INFO "MVVMM: Module loaded. Listening on IRQ %d, Port 0x%x\n", IRQ, PORT);
    return 0;
}

static void __exit mvvmm_exit(void)
{
    free_irq(IRQ, (void *)(my_irq_handler));
    unregister_reboot_notifier(&mvvmm_notifier);
    cancel_work_sync(&power_work);
    release_region(PORT, 1);
    printk(KERN_INFO "MVVMM: Module unloaded.\n");
}

module_init(mvvmm_init);
module_exit(mvvmm_exit);