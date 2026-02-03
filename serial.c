#include "serial.h"

#include <string.h>
#include <stdio.h>
#include <linux/kvm.h>
#include <linux/kvm_para.h>
#include <sys/ioctl.h>

#include "mvvm.h"

void serial_init(struct serial *self) {
    memset(self->regs, 0, sizeof(self->regs));
    self->dll = 0;
    self->dlh = 0;
    self->regs[2] = 0x01;
    self->regs[5] = 0x60;
    self->regs[6] = 0xB0;
}

void handle_serial(struct mvvm *vm, struct kvm_run *run) {
    uint8_t *io_data = (uint8_t *)run + run->io.data_offset;
    int offset = run->io.port - 0x3f8;
    struct serial *serial = &vm->serial;
    struct kvm_irq_level irq;
    int i;

    if (run->io.direction == KVM_EXIT_IO_OUT) {
        // Handle write operations
        // Transmit if DLAB bit is not set
        if (offset == 0 && !(serial->regs[3] & 0x80)) {
            // Output to stdout
            for (i = 0; i < run->io.count; i++) {
                putchar(io_data[i]);
            }
            fflush(stdout);
            // Trigger interrupt if enabled
            if (serial->regs[1] & 0x02) {
                serial->regs[2] = 0x02;
                irq.irq = 4;
                irq.level = 1;
                ioctl(vm->vm_fd, KVM_IRQ_LINE, &irq);
                irq.level = 0;
                ioctl(vm->vm_fd, KVM_IRQ_LINE, &irq);
            }
        }
        // Store value in register
        serial->regs[offset] = *io_data;
    } else {
        // Handle read operations
        switch (offset) {
        case 2: // IIR
            *io_data = serial->regs[2];
            // Clear THRE interrupt after read
            if ((serial->regs[2] & 0x0F) == 0x02) {
                serial->regs[2] = 0x01;
            }
            break;
        case 5: // LSR
            *io_data = 0x60;
            break;
        case 6: // MSR
            *io_data = 0xB0;
            break;
        default:
            *io_data = serial->regs[offset];
            break;
        }
    }
}