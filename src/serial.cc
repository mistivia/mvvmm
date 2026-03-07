/**
 * Copyright (c) 2026 Mistivia <i@mistivia.com>
 * 
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */


#include "serial.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include <linux/kvm.h>
#include <linux/kvm_para.h>
#include <sys/ioctl.h>

#include "mvvm.h"

namespace mvvmm {

void serial::clear_intr()
{
    m_regs[2] = 0b0001;
}

void serial::set_rx_intr()
{
    m_regs[2] = 0b0100;
}

int serial::is_rx_intr_set()
{
    return m_regs[2] == 0b0100;
}

void serial::set_tx_intr()
{
    m_regs[2] = 0b0010;
}

void serial::set_data_ready()
{
    m_regs[5] |= 0x01;
}

void serial::clear_data_ready()
{
    m_regs[5] &= (~0x01);
}

int serial::is_rx_empty()
{
    return !(m_regs[5] & 0x01);
}

int serial::is_dlab_set()
{
    return m_regs[3] & 0x80;
}

int serial::is_tx_intr_enabled()
{
    return m_regs[1] & 0b0010;
}

int serial::is_rx_intr_enabled()
{
    return m_regs[1] & 0b0001;
}

void serial::set_irq() {
    struct kvm_irq_level irq = {0};
    irq.irq = 4;
    irq.level = 1;
    if (ioctl(m_vm->vm_fd, KVM_IRQ_LINE, &irq) != 0) {
        fprintf(stderr, "failed to set irqline.\n");
    }
}

void serial::clear_irq()
{
    struct kvm_irq_level irq = {0};
    irq.irq = 4;
    irq.level = 0;
    if (ioctl(m_vm->vm_fd, KVM_IRQ_LINE, &irq) != 0) {
        fprintf(stderr, "failed to set irqline.\n");
    }
}

void serial::write_reg(int offset, uint8_t data)
{
    m_regs[offset] = data;
}

void write_to_serial(struct mvvm *vm, char c) {

}

uint8_t serial::read_reg(int offset) {
    if (offset == 7) return 0;
    if (offset == 2) {
        uint8_t ret = m_regs[2];
        if (is_rx_intr_set() && is_tx_intr_enabled()) {
            set_tx_intr();
        } else {
            clear_intr();
            clear_irq();
        }
        return ret;
    }
    if (offset == 0) {
        uint8_t ret = m_regs[0];
        clear_data_ready();
        m_rx_cond.notify_one();
        return ret;
    }
    return m_regs[offset];
}

serial::serial(mvvm *vm)
    : m_vm(vm)
{
    memset(m_regs, 0, sizeof(m_regs));
    m_regs[5] = 0x60;
    m_dl[0] = 0;
    m_dl[1] = 0;
    clear_intr();
}

void serial::handle_io_event(kvm_run *run)
{
    uint8_t *io_data = (uint8_t *)run + run->io.data_offset;
    int offset = run->io.port - 0x3f8;
    std::unique_lock<std::mutex> lk{m_rx_lock};

    if (is_dlab_set()) {
        if (offset == 1 || offset == 0) {
            if (run->io.direction == KVM_EXIT_IO_OUT) {
                m_dl[offset] = *io_data;
            } else {
                *io_data = m_dl[offset];
            }
        }
    }
    if (run->io.direction == KVM_EXIT_IO_OUT) {
        // Handle write operations
        if (offset == 0 && !is_dlab_set()) {
            putchar(*io_data);
            fflush(stdout);
            if (is_tx_intr_enabled()) {
                if (!is_rx_intr_set()) {
                    set_tx_intr();
                    set_irq();
                }
            }
        } else {
            write_reg(offset, *io_data);
        }
    } else {
        *io_data = read_reg(offset);
    }
}

void serial::write(char c)
{
    std::unique_lock<std::mutex> lk{m_rx_lock};
    while (!is_rx_empty()) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 3;
        auto cond_ret = m_rx_cond.wait_for(lk, std::chrono::seconds(3));
        if (cond_ret == std::cv_status::timeout) {
            return;
        }
    }
    m_regs[0] = c;
    if (is_rx_intr_enabled()) {
        set_rx_intr();
        set_irq();
    }
    set_data_ready();
}

} // namespace mvvmm
