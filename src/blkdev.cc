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

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include "virtio.h"
#include "threadpool.h"
#include "mvvm.h"
#include "config.h"

namespace mvvmm {

#define SECTOR_SIZE 512

class thread_pool;
// Context for block device operations using thread pool

struct block_device_ctx {
    int fd;
    uint64_t size;
    thread_pool *pool;
};

// Request structure passed to worker threads for async I/O
struct async_io_req {
    int fd;
    uint64_t offset;
    uint8_t *buf;
    size_t count;
    block_device_comp_func *cb;
    void *opaque;
    int is_write;
};

// Worker function executed by thread pool for disk I/O operations
static void *block_io_worker_fn(void *arg)
{
    struct async_io_req *req = (struct async_io_req *)arg;
    ssize_t n = 0;
    int ret = 0;

    // Perform actual I/O using pread/pwrite for thread safety
    if (req->is_write) {
        n = pwrite(req->fd, req->buf, req->count, req->offset);
        free(req->buf);
    } else {
        n = pread(req->fd, req->buf, req->count, req->offset);
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
        req->cb((struct blk_io_callback_arg *)req->opaque, ret);
    }

    free(req);
    return NULL;
}

// Get total sector count (synchronous operation)
static int64_t block_get_sector_count(block_device *bs)
{
    struct block_device_ctx *ctx = (struct block_device_ctx *)bs->opaque;
    return ctx->size / SECTOR_SIZE;
}

// Asynchronous read operation using thread pool
static int block_read_async(block_device *bs, uint64_t sector_num, uint8_t *buf, int n,
                            block_device_comp_func *cb, struct blk_io_callback_arg *opaque)
{
    struct block_device_ctx *ctx = (struct block_device_ctx *)bs->opaque;
    struct async_io_req *req = NULL;

    req = (struct async_io_req *)malloc(sizeof(*req));
    if (!req) {
        return -1;
    }

    req->fd = ctx->fd;
    req->offset = sector_num * SECTOR_SIZE;
    req->buf = buf;
    req->count = n * SECTOR_SIZE;
    req->cb = cb;
    req->opaque = opaque;
    req->is_write = 0;

    if (ctx->pool->run([req]() { block_io_worker_fn(req); }) < 0) {
        free(req);
        return -1;
    }

    return 0;
}

// Asynchronous write operation using thread pool
static int block_write_async(block_device *bs, uint64_t sector_num, const uint8_t *buf, int n,
                             block_device_comp_func *cb, struct blk_io_callback_arg *opaque)
{
    struct block_device_ctx *ctx = (struct block_device_ctx *)bs->opaque;
    struct async_io_req *req = NULL;
    ;

    req = (struct async_io_req *)malloc(sizeof(*req));
    if (!req) {
        return -ENOMEM;
    }

    req->fd = ctx->fd;
    req->offset = sector_num * SECTOR_SIZE;
    // Cast away const for pread/pwrite API compatibility
    req->buf = (uint8_t *)buf;
    req->count = n * SECTOR_SIZE;
    req->cb = cb;
    req->opaque = opaque;
    req->is_write = 1;

    if (ctx->pool->run([req]() { block_io_worker_fn(req); }) < 0) {
        free(req);
        return -1;
    }

    return 0;
}

// Initialize virtio block device with thread pool backend
int mvvm_init_virtio_blk(struct mvvm *self, const char *disk_path)
{
    struct block_device_ctx *ctx = NULL;
    block_device *bs = NULL;
    struct irq_signal irq = {0};
    struct stat st = {0};
    virtio_bus_def bus = {0};
    int ret = -1;
    // Allocate block device context
    ctx = (struct block_device_ctx *)malloc(sizeof(*ctx));
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
    ctx->pool = thread_pool::make_instance(VIRTIO_BLK_MAX_QUEUE_NUM);
    if (!ctx->pool) {
        fprintf(stderr, "failed to create thread pool\n");
        goto fail;
    }
    // Allocate and initialize block_device structure
    bs = (block_device *)malloc(sizeof(*bs));
    if (!bs) {
        fprintf(stderr, "failed to allocate block_device structure\n");
        goto fail;
    }

    bs->get_sector_count = block_get_sector_count;
    bs->read_async = block_read_async;
    bs->write_async = block_write_async;
    bs->opaque = ctx;

    irq.vmfd = self->vm_fd;
    irq.irqline = VIRTIO_BLK_IRQ;
    // Setup virtio bus definition
    bus.mem_map = self->mem_map;
    bus.irq = irq;
    // Initialize virtio block device
    self->blk = virtio_block_init(bus, VIRTIO_BLK_MMIO_ADDR, bs);
    if (!self->blk) {
        fprintf(stderr, "failed to initialize virtio block device\n");
        goto fail;
    }
    // Note: mem_map and irq are now owned by virtio device layer
    // ctx and bs are referenced by virtio device for callbacks
    return 0;

fail:
    if (bs) {
        free(bs);
    }
    if (ctx) {
        if (ctx->fd >= 0) {
            close(ctx->fd);
        }
        free(ctx);
    }
    return ret;
}

void mvvm_destroy_virtio_blk(struct mvvm *self)
{
    struct block_device_ctx *ctx = (struct block_device_ctx *)virtio_block_get_opaque(self->blk);
    delete ctx->pool;
    free(ctx);
    virtio_block_destroy(self->blk);
    free(self->blk);
}

} // namespace mvvmm
