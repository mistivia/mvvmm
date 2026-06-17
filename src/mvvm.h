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

#include <stdlib.h>

#include "blkdev.h"
#include "netdev.h"
#include "virtio.h"
#include "serial.h"

extern "C" {
    struct boot_params;
    struct kvm_segment;
}

namespace mvvmm {

struct guest_mem_map {
    explicit guest_mem_map() = default;
    uint8_t* addr_to_host(uint64_t addr);
    void *host_mem;
    uint64_t size;
};

class mvvm {
public:
    int init(uint64_t mem_size, const char *disk, const char *network);
    int load_kernel(const char *kernel_path,
                    const char *initrd_path, const char *kernel_args);
    int run();
    void set_quit(bool q) { m_quit = q; }
    void shutdown();
    int set_irq(int irq, int level);
    ~mvvm();
private:
    static int init_cpu(int kvm_fd, int cpu_fd);
    static void set_flat_mode(struct kvm_segment *seg);
    void setup_e820_map(struct boot_params *zeropage);
    int load_initrd(struct boot_params *zeropage, const char *initrd_path);
    int handle_power(struct kvm_run *run);

    int m_kvm_fd = -1;
    int m_vm_fd = -1;
    int m_cpu_fd = -1;
    std::unique_ptr<guest_mem_map> m_mem_map{};
    std::unique_ptr<mvvmm::serial> m_serial{};
    std::unique_ptr<virtio_device> m_blk{};
    std::unique_ptr<virtio_device> m_net{};
    int m_quit = 0;
    uint8_t m_power_cmd = 0;

    friend void keyboard_thread_func(mvvm *vm);
    friend class block_device_impl;
    friend class tap_net_impl;
};

} // namespace mvvmm
