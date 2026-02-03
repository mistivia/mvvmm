#include "serial.h"

#include <string.h>
#include <stdio.h>
#include <stdint.h>

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

static inline int is_dlab_set(struct serial *self) {
    return self->regs[3] & 0x80;
}

static inline void trigger_serial_intr(struct mvvm *vm) {
    struct serial *serial = &vm->serial;
    struct kvm_irq_level irq;
    if (serial->regs[1] & 0x02) {
        serial->regs[2] = 0x02;
        irq.irq = 4;
        irq.level = 1;
        ioctl(vm->vm_fd, KVM_IRQ_LINE, &irq);
        irq.level = 0;
        ioctl(vm->vm_fd, KVM_IRQ_LINE, &irq);
    }
}

static inline void write_reg(struct serial *self, int offset, uint8_t data) {
    self->regs[offset] = data;
}

static inline uint8_t read_reg(struct serial *self, int offset) {
    uint8_t ret;
    switch (offset) {
    case 2: // IIR
        ret = self->regs[2];
        // Clear THRE interrupt after read
        if ((self->regs[2] & 0x0F) == 0x02) {
            self->regs[2] = 0x01;
        }
        return ret;
    default:
        return self->regs[offset];
    }
}

void handle_serial(struct mvvm *vm, struct kvm_run *run) {
    uint8_t *io_data = (uint8_t *)run + run->io.data_offset;
    int offset = run->io.port - 0x3f8;
    struct serial *serial = &vm->serial;
    if (run->io.direction == KVM_EXIT_IO_OUT) {
        // Handle write operations
        if (offset == 0 && !is_dlab_set(serial)) {
            // Output to stdout
            for (int i = 0; i < run->io.count; i++) {
                putchar(io_data[i]);
            }
            fflush(stdout);
            trigger_serial_intr(vm);
        } else {
            write_reg(serial, offset, *io_data);
        }
    } else {
        *io_data = read_reg(serial, offset);
    }
}