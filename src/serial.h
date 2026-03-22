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

#pragma once

#include <mutex>
#include <condition_variable>
#include <cstdint>

struct kvm_run;

namespace mvvmm {

struct mvvm;

class serial {
public:
    explicit serial(mvvm *vm);
    void handle_io_event(kvm_run *run);
    void write(char c);
    
private:
    uint8_t m_regs[8];
    uint8_t m_dl[2];
    std::mutex m_rx_lock;
    std::condition_variable m_rx_cond;
    mvvm *m_vm = nullptr;

    void clear_intr() { m_regs[2] = 0b0001; }
    void set_rx_intr() { m_regs[2] = 0b0100; }
    int is_rx_intr_set() { return m_regs[2] == 0b0100; }
    void set_tx_intr() { m_regs[2] = 0b0010; }
    void set_data_ready() { m_regs[5] |= 0x01; }
    void clear_data_ready() { m_regs[5] &= (~0x01); }
    int is_rx_empty() { return !(m_regs[5] & 0x01); }
    int is_dlab_set() { return m_regs[3] & 0x80; }
    int is_tx_intr_enabled() { return m_regs[1] & 0b0010; }
    int is_rx_intr_enabled() { return m_regs[1] & 0b0001; }
    void set_irq();
    void clear_irq();
    void write_reg(int offset, uint8_t data);
    uint8_t read_reg(int offset);
};

} // namespace mvvmm
