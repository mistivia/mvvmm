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

#include <memory>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include "blkdev.h"
#include "mvvm.h"
#include "config.h"
#include "virtio.h"

namespace mvvmm {

#define SECTOR_SIZE 512

class thread_pool;

block_device_impl::~block_device_impl() {
    if (m_fd >= 0) {
        close(m_fd);
    }
}

// Request structure passed to worker threads for async I/O
struct async_io_req {
    explicit async_io_req() = default;
    int fd = -1;
    uint64_t offset = 0;
    size_t count = 0;
    block_device_comp_func cb = nullptr;
    std::unique_ptr<blk_io_callback_arg> arg;
    int is_write = 0;
};

// Worker function executed by thread pool for disk I/O operations
static void block_io_worker_fn(std::shared_ptr<async_io_req> req)
{
    ssize_t n = 0;
    int ret = 0;

    // Perform actual I/O using pread/pwrite for thread safety
    if (req->is_write) {
        n = pwrite(req->fd, req->arg->req.buf.get(), req->count, req->offset);
    } else {
        n = pread(req->fd, req->arg->req.buf.get(), req->count, req->offset);
    }

    // Determine return code based on operation result
    if (n < 0) {
        ret = -1;
    } else if ((size_t)n != req->count) {
        ret = -1;
    } else {
        ret = 0;
    }

    // Invoke completion callback if provided
    if (req->cb) {
        req->cb(std::move(req->arg), ret);
    }
}

// Get total sector count (synchronous operation)
int64_t block_device_impl::get_sector_count()
{
    return m_size / SECTOR_SIZE;
}

// Asynchronous read operation using thread pool
void block_device_impl::read_async(uint64_t sector_num, int n,
                            block_device_comp_func cb, std::unique_ptr<blk_io_callback_arg> arg)
{
    auto req = std::make_shared<async_io_req>();

    req->fd = m_fd;
    req->offset = sector_num * SECTOR_SIZE;
    req->count = n * SECTOR_SIZE;
    req->cb = cb;
    req->arg = std::move(arg);
    req->is_write = 0;
    if (m_pool->run([req]() {block_io_worker_fn(req);}) < 0) {
        virtio_block_req_end(std::move(req->arg), -1);
        return;
    }

    return;
}

// Asynchronous write operation using thread pool
void block_device_impl::write_async(uint64_t sector_num, int n,
                                    block_device_comp_func cb,
                                    std::unique_ptr<blk_io_callback_arg> arg)
{
    auto req = std::make_shared<async_io_req>();

    req->fd = m_fd;
    req->offset = sector_num * SECTOR_SIZE;
    // Cast away const for pread/pwrite API compatibility
    req->count = n * SECTOR_SIZE;
    req->cb = cb;
    req->arg = std::move(arg);
    req->is_write = 1;
    if (m_pool->run([req]() { block_io_worker_fn(req); }) < 0) {
        virtio_block_req_end(std::move(req->arg), -1);
        return;
    }

    return;
}

// Initialize virtio block device with thread pool backend
int block_device_impl::init(mvvm *vm, const char *disk_path)
{
    struct stat st = {0};
    virtio_bus_def bus{};
    // Open disk image file
    m_fd = open(disk_path, O_RDWR);
    if (m_fd < 0) {
        perror("block_device_impl::init, open disk image");
        return -1;
    }
    // Get file size
    if (fstat(m_fd, &st) < 0) {
        perror("block_device_impl::init, fstat disk image");
        return -1;
    }
    m_size = st.st_size;
    // Create thread pool for async I/O operations
    m_pool = thread_pool::make_instance(VIRTIO_BLK_MAX_QUEUE_NUM);
    if (!m_pool) {
        fprintf(stderr, "failed to create thread pool\n");
        return -1;
    }

    bus.vmfd = vm->m_vm_fd;
    bus.irqline = VIRTIO_BLK_IRQ;
    bus.mem_map = vm->m_mem_map.get();
    // Initialize virtio block device
    vm->m_blk = virtio_block_init(bus, VIRTIO_BLK_MMIO_ADDR, shared_from_this());
    if (!vm->m_blk) {
        fprintf(stderr, "failed to initialize virtio block device\n");
        return -1;
    }
    // Note: mem_map and irq are now owned by virtio device layer
    // ctx and bs are referenced by virtio device for callbacks
    return 0;
}

} // namespace mvvmm
