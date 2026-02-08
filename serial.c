#include "serial.h"

#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>

#include <linux/kvm.h>
#include <linux/kvm_para.h>
#include <sys/ioctl.h>

#include "mvvm.h"

static inline void clear_intr(struct serial *self) {
    self->regs[2] = 0b0001;
}

static inline void set_rx_intr(struct serial *self) {
    self->regs[2] = 0b0100;
}

static inline int is_rx_intr_set(struct serial *self) {
    return self->regs[2] == 0b0100;
}

static inline void set_tx_intr(struct serial *self) {
    self->regs[2] = 0b0010;
}

static inline void set_data_ready(struct serial *self) {
    self->regs[5] |= 0x01;
}

static inline void clear_data_ready(struct serial *self) {
    self->regs[5] &= (~0x01);
}

static inline int is_rx_empty(struct serial *self) {
    return !(self->regs[5] & 0x01);
}

static inline int is_dlab_set(struct serial *self) {
    return self->regs[3] & 0x80;
}

static inline int is_tx_intr_enabled(struct serial *self) {
    return self->regs[1] & 0b0010;
}

static inline int is_rx_intr_enabled(struct serial *self) {
    return self->regs[1] & 0b0001;
}

static inline void trigger_serial_intr(struct mvvm *vm) {
    struct kvm_irq_level irq = {0};
    irq.irq = 4;
    irq.level = 1;
    if (ioctl(vm->vm_fd, KVM_IRQ_LINE, &irq) != 0) {
        fprintf(stderr, "failed to set irqline.\n");
    }
    irq.level = 0;
    if (ioctl(vm->vm_fd, KVM_IRQ_LINE, &irq) != 0) {
        fprintf(stderr, "failed to set irqline.\n");
    }
}

void serial_init(struct serial *self) {
    memset(self->regs, 0, sizeof(self->regs));
    self->regs[5] = 0x60;
    self->dl[0] = 0;
    self->dl[1] = 0;
    clear_intr(self);
    pthread_mutex_init(&self->rx_lock, NULL);
    pthread_cond_init(&self->rx_cond, NULL);
}

static inline void write_reg(struct serial *self, int offset, uint8_t data) {
    self->regs[offset] = data;
}

void write_to_serial(struct mvvm *vm, char c) {
    struct serial *serial = &vm->serial;
    pthread_mutex_lock(&serial->rx_lock);
    while (!is_rx_empty(serial)) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 3;
        int ret = pthread_cond_timedwait(&serial->rx_cond, &serial->rx_lock, &ts);
        if (ret == ETIMEDOUT) {
            goto end;
        }
    }
    serial->regs[0] = c;
    if (is_rx_intr_enabled(serial)) {
        set_rx_intr(serial);
        trigger_serial_intr(vm);
    }
    set_data_ready(serial);
end:
    pthread_mutex_unlock(&serial->rx_lock);
}

static uint8_t read_reg(struct serial *self, int offset) {
    if (offset == 7) return 0;
    if (offset == 2) {
        uint8_t ret = self->regs[2];
        if (is_rx_intr_set(self) && is_tx_intr_enabled(self)) {
            set_tx_intr(self);
        } else {
            clear_intr(self);
        }
        return ret;
    }
    if (offset == 0) {
        uint8_t ret = self->regs[0];
        clear_data_ready(self);
        pthread_cond_signal(&self->rx_cond);
        return ret;
    }
    return self->regs[offset];
}

void handle_serial(struct mvvm *vm, struct kvm_run *run) {
    uint8_t *io_data = (uint8_t *)run + run->io.data_offset;
    int offset = run->io.port - 0x3f8;
    struct serial *serial = &vm->serial;
    pthread_mutex_lock(&serial->rx_lock);

    if (is_dlab_set(serial)) {
        if (offset == 1 || offset == 0) {
            if (run->io.direction == KVM_EXIT_IO_OUT) {
                serial->dl[offset] = *io_data;
            } else {
                *io_data = serial->dl[offset];
            }
        }
    }
    if (run->io.direction == KVM_EXIT_IO_OUT) {
        // Handle write operations
        if (offset == 0 && !is_dlab_set(serial)) {
            putchar(*io_data);
            fflush(stdout);
            if (is_tx_intr_enabled(serial)) {
                if (!is_rx_intr_set(serial)) {
                    set_tx_intr(serial);
                    trigger_serial_intr(vm);
                }
            }
        } else {
            write_reg(serial, offset, *io_data);
        }
    } else {
        *io_data = read_reg(serial, offset);
    }
    pthread_mutex_unlock(&serial->rx_lock);
}