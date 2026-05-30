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

#include "virtio.h"

namespace mvvmm {

class mvvm;

struct block_device_impl : public block_device {
    explicit block_device_impl() = default;
    virtual ~block_device_impl() override;
    int fd = -1;
    uint64_t size = 0;
    std::unique_ptr<thread_pool> pool;

    virtual int64_t get_sector_count() override;
    virtual void read_async(uint64_t sector_num, int n, // n is sector number
                            block_device_comp_func cb, std::unique_ptr<blk_io_callback_arg> cbarg) override;
    virtual void write_async(uint64_t sector_num, int n, // n is sector nubmer
                             block_device_comp_func cb, std::unique_ptr<blk_io_callback_arg> cbarg) override;
};

int
mvvm_init_virtio_blk(mvvm *self, const char *disk_path);

} // namespace mvvmm
