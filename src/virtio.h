/*
 * VIRTIO device
 * 
 * Copyright (c) 2016 Fabrice Bellard
 * Copyright (c) 2026 Mistivia <i@mistivia.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

namespace mvvmm {

typedef uint64_t virtio_phys_addr_t;

struct irq_signal {
    int vmfd;
    int irqline;
    int irqfd;  /* eventfd for irqfd mechanism */
};
typedef struct irq_signal irq_signal;

typedef struct {
    struct guest_mem_map *mem_map;
    irq_signal irq;
} virtio_bus_def;

/* irqfd functions */
int virtio_irqfd_init(irq_signal *irq);
void virtio_irqfd_cleanup(irq_signal *irq);

typedef struct virtio_device virtio_device; 

uint32_t virtio_mmio_read(virtio_device *s, uint32_t offset1, int size);
void virtio_mmio_write(virtio_device *s, uint32_t offset,
                       uint32_t val, int size);

void virtio_set_debug(virtio_device *s, int debug_flags);

/* block device */
struct blk_io_callback_arg;
typedef void block_device_comp_func(struct blk_io_callback_arg *callback_arg, int ret);

struct disk_image {
    int fd;
    uint64_t size;
};

typedef struct block_device block_device;

typedef struct {
    uint32_t type;
    uint8_t *buf;
    int write_size;
    int queue_idx;
    int desc_idx;
} block_request;

struct blk_io_callback_arg {
    virtio_device *s;
    block_request req;
};

struct block_device {
    int64_t (*get_sector_count)(block_device *bs);
    int (*read_async)(block_device *bs,
                      uint64_t sector_num, uint8_t *buf, int n, // n is sector number
                      block_device_comp_func *cb, struct blk_io_callback_arg *cbarg);
    int (*write_async)(block_device *bs,
                       uint64_t sector_num, const uint8_t *buf, int n, // n is sector nubmer
                       block_device_comp_func *cb, struct blk_io_callback_arg *cbarg);
    void *opaque;
};

virtio_device *virtio_block_init(virtio_bus_def bus, uint64_t mmio_addr, block_device *bs);

void virtio_block_destroy(virtio_device *s);
void* virtio_block_get_opaque(virtio_device *s);

/* network device */
struct ethernet_device;
typedef struct ethernet_device ethernet_device; 

struct ethernet_device {
    uint8_t mac_addr[6]; /* mac address of the interface */
    void (*write_packet_to_ether)(ethernet_device *net,
                                  const uint8_t *buf, int len);
    void *opaque;
    /* the following is set by the device */
    void *device_opaque;
    bool (*can_write_packet_to_virtio)(ethernet_device *net);
    void (*write_packet_to_virtio)(ethernet_device *net,
                                const uint8_t *buf, int len);

};

virtio_device *virtio_net_init(virtio_bus_def bus, uint64_t mmio_addr, ethernet_device *es);

void virtio_net_destroy(virtio_device *s);
void* virtio_net_get_opaque(virtio_device *s);

} // namespace mvvmm
