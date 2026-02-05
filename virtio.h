/*
 * VIRTIO driver
 * 
 * Copyright (c) 2016 Fabrice Bellard
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

#define VIRTIO_PAGE_SIZE 4096

typedef uint64_t virtio_phys_addr_t;

struct PhysMemoryMap;
typedef struct PhysMemoryMap PhysMemoryMap;
struct IRQSignal;
typedef struct IRQSignal IRQSignal;

typedef struct {
    PhysMemoryMap *mem_map;
    uint64_t addr;
    IRQSignal *irq;
} VIRTIOBusDef;

typedef struct VIRTIODevice VIRTIODevice; 

void virtio_set_debug(VIRTIODevice *s, int debug_flags);

/* block device */

typedef void BlockDeviceCompletionFunc(void *opaque, int ret);

typedef struct BlockDevice BlockDevice;

struct BlockDevice {
    int64_t (*get_sector_count)(BlockDevice *bs);
    int (*read_async)(BlockDevice *bs,
                      uint64_t sector_num, uint8_t *buf, int n,
                      BlockDeviceCompletionFunc *cb, void *opaque);
    int (*write_async)(BlockDevice *bs,
                       uint64_t sector_num, const uint8_t *buf, int n,
                       BlockDeviceCompletionFunc *cb, void *opaque);
    void *opaque;
};

VIRTIODevice *virtio_block_init(VIRTIOBusDef *bus, BlockDevice *bs);

/* network device */
struct EthernetDevice;
typedef struct EthernetDevice EthernetDevice; 

struct EthernetDevice {
    uint8_t mac_addr[6]; /* mac address of the interface */
    void (*write_packet)(EthernetDevice *net,
                         const uint8_t *buf, int len);
    void *opaque;
    /* the following is set by the device */
    void *device_opaque;
    bool (*device_can_write_packet)(EthernetDevice *net);
    void (*device_write_packet)(EthernetDevice *net,
                                const uint8_t *buf, int len);
    void (*device_set_carrier)(EthernetDevice *net, bool carrier_state);
};

VIRTIODevice *virtio_net_init(VIRTIOBusDef *bus, EthernetDevice *es);

#endif /* VIRTIO_H */
