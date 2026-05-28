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
#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include "virtio.h"
#include "mvvm.h"
#include "config.h"

namespace mvvmm {

#define SECTOR_SIZE 512

class thread_pool;

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
static void block_io_worker_fn(std::unique_ptr<async_io_req> req)
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
static int64_t block_get_sector_count(block_device *bs)
{
    return bs->ctx->size / SECTOR_SIZE;
}

// Asynchronous read operation using thread pool
static void block_read_async(block_device *bs, uint64_t sector_num, int n,
                            block_device_comp_func cb, std::unique_ptr<blk_io_callback_arg> arg)
{
    auto req = std::make_unique<async_io_req>();

    req->fd = bs->ctx->fd;
    req->offset = sector_num * SECTOR_SIZE;
    req->count = n * SECTOR_SIZE;
    req->cb = cb;
    req->arg = std::move(arg);
    req->is_write = 0;
    auto rawreq = req.release();
    int runret = bs->ctx->pool->run([rawreq]() { 
        block_io_worker_fn(std::unique_ptr<async_io_req>{rawreq});
    });
    if (runret < 0) {
        // unsafe
        req = std::unique_ptr<async_io_req>(rawreq);
        virtio_block_req_end(std::move(req->arg), -1);
        return;
    }

    return;
}

// Asynchronous write operation using thread pool
static void block_write_async(block_device *bs, uint64_t sector_num, int n,
                              block_device_comp_func cb, std::unique_ptr<blk_io_callback_arg> arg)
{
    auto req = std::make_unique<async_io_req>();

    req->fd = bs->ctx->fd;
    req->offset = sector_num * SECTOR_SIZE;
    // Cast away const for pread/pwrite API compatibility
    req->count = n * SECTOR_SIZE;
    req->cb = cb;
    req->arg = std::move(arg);
    req->is_write = 1;
    auto rawreq = req.release();
    if (bs->ctx->pool->run([rawreq]() { block_io_worker_fn(std::unique_ptr<async_io_req>(rawreq)); }) < 0) {
        // unsafe
        req = std::unique_ptr<async_io_req>(rawreq);
        virtio_block_req_end(std::move(req->arg), -1);
        return;
    }

    return;
}

// Initialize virtio block device with thread pool backend
int mvvm_init_virtio_blk(mvvm *self, const char *disk_path)
{
    block_device *bs = NULL;
    struct stat st = {0};
    virtio_bus_def bus{};
    int ret = -1;
    // Allocate block device context
    auto ctx = std::make_unique<block_device_ctx>();
    if (!ctx) {
        fprintf(stderr, "failed to allocate block device context\n");
        return -1;
    }
    // Open disk image file
    ctx->fd = open(disk_path, O_RDWR);
    if (ctx->fd < 0) {
        perror("mvvm_init_virtio_blk, open disk image");
        goto fail;
    }
    // Get file size
    if (fstat(ctx->fd, &st) < 0) {
        perror("mvvm_init_virtio_blk, fstat disk image");
        goto fail;
    }
    ctx->size = st.st_size;
    // Create thread pool for async I/O operations
    ctx->pool = std::unique_ptr<thread_pool>(thread_pool::make_instance(VIRTIO_BLK_MAX_QUEUE_NUM));
    if (!ctx->pool) {
        fprintf(stderr, "failed to create thread pool\n");
        goto fail;
    }
    // Allocate and initialize block_device structure
    bs = new block_device{};
    if (!bs) {
        fprintf(stderr, "failed to allocate block_device structure\n");
        goto fail;
    }

    bs->get_sector_count = block_get_sector_count;
    bs->read_async = block_read_async;
    bs->write_async = block_write_async;
    bs->ctx = std::move(ctx);

    bus.vmfd = self->m_vm_fd;
    bus.irqline = VIRTIO_BLK_IRQ;
    bus.mem_map = self->m_mem_map;
    // Initialize virtio block device
    self->m_blk = virtio_block_init(bus, VIRTIO_BLK_MMIO_ADDR, bs);
    if (!self->m_blk) {
        fprintf(stderr, "failed to initialize virtio block device\n");
        goto fail;
    }
    // Note: mem_map and irq are now owned by virtio device layer
    // ctx and bs are referenced by virtio device for callbacks
    return 0;

fail:
    if (bs) {
        delete bs;
    }
    return ret;
}

void mvvm_destroy_virtio_blk(mvvm *self)
{
    virtio_block_destroy(self->m_blk);
    delete self->m_blk;
}

} // namespace mvvmm
