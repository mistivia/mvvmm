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

#include "virtio.h"
#include "serial.h"

namespace mvvmm {

struct guest_mem_map {
    void *host_mem;
    uint64_t size;
};

struct mvvm {
    int kvm_fd;
    int vm_fd;
    int cpu_fd;
    struct guest_mem_map *mem_map;
    std::unique_ptr<mvvmm::serial> serial;
    virtio_device *blk;
    virtio_device *net;
    int quit;
    uint8_t power_cmd;
};

int mvvm_init(struct mvvm *vm, uint64_t mem_size, const char *disk, const char *network);
int init_cpu(int kvm_fd, int cpu_fd);
int mvvm_load_kernel(struct mvvm *vm, const char *kernel_path,
                     const char *initrd_path, const char *kernel_args);
int mvvm_run(struct mvvm *vm);
void mvvm_destroy(struct mvvm *self);

// must use with mvvmm guest module
void mvvm_shutdown(struct mvvm *vm);

} // namespace mvvmm
