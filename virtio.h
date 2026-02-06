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
#ifndef VIRTIO_H
#define VIRTIO_H

#include <stdint.h>
#include <stdbool.h>

typedef uint64_t virtio_phys_addr_t;

struct PhysMemoryMapEntry {
    void *host_mem;
    uint64_t guest_addr;
    uint64_t size;
};

struct PhysMemoryMap {
    int size;
    struct PhysMemoryMapEntry entries[0];
};

struct IRQSignal {
    int vmfd;
    int irqline;
};
typedef struct IRQSignal IRQSignal;

typedef struct {
    struct PhysMemoryMap *mem_map;
    IRQSignal *irq;
} VIRTIOBusDef;

typedef struct VIRTIODevice VIRTIODevice; 

void virtio_config_change_notify(VIRTIODevice *s);
uint32_t virtio_mmio_read(VIRTIODevice *s, uint32_t offset1, int size);
void virtio_mmio_write(VIRTIODevice *s, uint32_t offset,
                       uint32_t val, int size);

void virtio_set_debug(VIRTIODevice *s, int debug_flags);

/* block device */

typedef void BlockDeviceCompletionFunc(void *opaque, int ret);

struct disk_image {
    int fd;
    uint64_t size;
};

typedef struct BlockDevice BlockDevice;

struct BlockDevice {
    int64_t (*get_sector_count)(BlockDevice *bs);
    int (*read_async)(BlockDevice *bs,
                      uint64_t sector_num, uint8_t *buf, int n, // n is sector number
                      BlockDeviceCompletionFunc *cb, void *opaque);
    int (*write_async)(BlockDevice *bs,
                       uint64_t sector_num, const uint8_t *buf, int n, // n is sector nubmer
                       BlockDeviceCompletionFunc *cb, void *opaque);
    void *opaque;
};

VIRTIODevice *virtio_block_init(VIRTIOBusDef *bus, BlockDevice *bs);

/* network device */
struct EthernetDevice;
typedef struct EthernetDevice EthernetDevice; 

struct EthernetDevice {
    uint8_t mac_addr[6]; /* mac address of the interface */
    void (*write_packet_to_ether)(EthernetDevice *net,
                                  const uint8_t *buf, int len);
    void *opaque;
    /* the following is set by the device */
    void *device_opaque;
    bool (*can_write_packet_to_virtio)(EthernetDevice *net);
    void (*write_packet_to_virtio)(EthernetDevice *net,
                                const uint8_t *buf, int len);

};

VIRTIODevice *virtio_net_init(VIRTIOBusDef *bus, EthernetDevice *es);

#endif /* VIRTIO_H */
