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

#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>
#include <stdarg.h>

#include "virtio.h"

//#define DEBUG_VIRTIO

/* MMIO addresses - from the Linux kernel */
#define VIRTIO_MMIO_MAGIC_VALUE		0x000
#define VIRTIO_MMIO_VERSION		0x004
#define VIRTIO_MMIO_DEVICE_ID		0x008
#define VIRTIO_MMIO_VENDOR_ID		0x00c
#define VIRTIO_MMIO_DEVICE_FEATURES	0x010
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL	0x014
#define VIRTIO_MMIO_DRIVER_FEATURES	0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL	0x024

#define VIRTIO_MMIO_QUEUE_SEL		0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX	0x034
#define VIRTIO_MMIO_QUEUE_NUM		0x038

#define VIRTIO_MMIO_QUEUE_READY		0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY	0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS	0x060
#define VIRTIO_MMIO_INTERRUPT_ACK	0x064
#define VIRTIO_MMIO_STATUS		0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW	0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH	0x084
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW	0x090
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH	0x094
#define VIRTIO_MMIO_QUEUE_USED_LOW	0x0a0
#define VIRTIO_MMIO_QUEUE_USED_HIGH	0x0a4
#define VIRTIO_MMIO_CONFIG_GENERATION	0x0fc
#define VIRTIO_MMIO_CONFIG		0x100

#define MAX_QUEUE 8
#define MAX_CONFIG_SPACE_SIZE 256
#define MAX_QUEUE_NUM 16

typedef struct {
    uint32_t ready; /* 0 or 1 */
    uint32_t num;
    uint16_t last_avail_idx;
    virtio_phys_addr_t desc_addr;
    virtio_phys_addr_t avail_addr;
    virtio_phys_addr_t used_addr;
    bool manual_recv; /* if true, the device_recv() callback is not called */
} QueueState;

#define VRING_DESC_F_NEXT	1
#define VRING_DESC_F_WRITE	2
#define VRING_DESC_F_INDIRECT	4

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags; /* VRING_DESC_F_x */
    uint16_t next;
} VIRTIODesc;

/* return < 0 to stop the notification (it must be manually restarted
   later), 0 if OK */
typedef int VIRTIODeviceRecvFunc(VIRTIODevice *s1, int queue_idx,
                                 int desc_idx, int read_size,
                                 int write_size);

/* return NULL if no RAM at this address. The mapping is valid for one page */
typedef uint8_t *VIRTIOGetRAMPtrFunc(VIRTIODevice *s, virtio_phys_addr_t paddr);

struct VIRTIODevice {
    PhysMemoryMap *mem_map;
    /* MMIO only */
    IRQSignal *irq;
    VIRTIOGetRAMPtrFunc *get_ram_ptr;
    int debug;

    uint32_t int_status;
    uint32_t status;
    uint32_t device_features_sel;
    uint32_t queue_sel; /* currently selected queue */
    QueueState queue[MAX_QUEUE];

    /* device specific */
    uint32_t device_id;
    uint32_t vendor_id;
    uint32_t device_features;
    VIRTIODeviceRecvFunc *device_recv;
    void (*config_write)(VIRTIODevice *s); /* called after the config
                                              is written */
    uint32_t config_space_size; /* in bytes, must be multiple of 4 */
    uint8_t config_space[MAX_CONFIG_SPACE_SIZE];
    pthread_mutex_t lock;
};


static void put_le32(void* ptr, uint32_t val)
{
    if (ptr == NULL) {
        return;
    }
    uint8_t* const p = (uint8_t* const)ptr;
    p[0] = (uint8_t)(val & 0xFF);
    p[1] = (uint8_t)((val >> 8) & 0xFF);
    p[2] = (uint8_t)((val >> 16) & 0xFF);
    p[3] = (uint8_t)((val >> 24) & 0xFF);
}

static void put_le16(void* ptr, uint16_t val)
{
    if (ptr == NULL) {
        return;
    }
    uint8_t* const p = (uint8_t* const)ptr;
    p[0] = (uint8_t)(val & 0xFF);
    p[1] = (uint8_t)((val >> 8) & 0xFF);
}

static uint32_t get_le32(void *ptr)
{
    if (ptr == NULL) {
        return 0;
    }
    const uint8_t* const p = (const uint8_t* const)ptr;
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t get_le16(void *ptr)
{
    if (ptr == NULL) {
        return 0;
    }
    const uint8_t* const p = (const uint8_t* const)ptr;
    return ((uint16_t)p[0]) |
           ((uint16_t)p[1] << 8);
}

static void virtio_reset(VIRTIODevice *s)
{
    int i = 0;

    s->status = 0;
    s->queue_sel = 0;
    s->device_features_sel = 0;
    s->int_status = 0;
    for(i = 0; i < MAX_QUEUE; i++) {
        QueueState *qs = &s->queue[i];
        qs->ready = 0;
        qs->num = MAX_QUEUE_NUM;
        qs->desc_addr = 0;
        qs->avail_addr = 0;
        qs->used_addr = 0;
        qs->last_avail_idx = 0;
    }
}

static uint8_t* guest_addr_to_host_addr(VIRTIODevice *s, uint64_t guest_addr) {
    // TODO
    return NULL;
}

static void virtio_init(VIRTIODevice *s, VIRTIOBusDef *bus,
                        uint32_t device_id, int config_space_size,
                        VIRTIODeviceRecvFunc *device_recv)
{
    memset(s, 0, sizeof(*s));

    s->mem_map = bus->mem_map;
    s->irq = bus->irq;
    s->get_ram_ptr = guest_addr_to_host_addr;

    s->device_id = device_id;
    s->vendor_id = 0xffff;
    s->config_space_size = config_space_size;
    s->device_recv = device_recv;
    pthread_mutex_init(&s->lock, NULL);
    virtio_reset(s);
}

static uint16_t virtio_read16(VIRTIODevice *s, virtio_phys_addr_t addr)
{
    uint8_t *ptr = NULL;
    if (addr & 1)
        return 0; /* unaligned access are not supported */
    ptr = s->get_ram_ptr(s, addr);
    if (!ptr)
        return 0;
    return *(uint16_t *)ptr;
}

static void virtio_write16(VIRTIODevice *s, virtio_phys_addr_t addr,
                           uint16_t val)
{
    uint8_t *ptr = NULL;
    if (addr & 1)
        return; /* unaligned access are not supported */
    ptr = s->get_ram_ptr(s, addr);
    if (!ptr)
        return;
    *(uint16_t *)ptr = val;
}

static void virtio_write32(VIRTIODevice *s, virtio_phys_addr_t addr,
                           uint32_t val)
{
    uint8_t *ptr = NULL;
    if (addr & 3)
        return; /* unaligned access are not supported */
    ptr = s->get_ram_ptr(s, addr);
    if (!ptr)
        return;
    *(uint32_t *)ptr = val;
}

static inline int min_int(int a, int b) {
    return a < b ? a : b;
}

static int virtio_memcpy_from_ram(VIRTIODevice *s, uint8_t *buf,
                                  virtio_phys_addr_t addr, int count)
{
    uint8_t *ptr = NULL;
    int l = {0};

    while (count > 0) {
        l = min_int(count, VIRTIO_PAGE_SIZE - (addr & (VIRTIO_PAGE_SIZE - 1)));
        ptr = s->get_ram_ptr(s, addr);
        if (!ptr)
            return -1;
        memcpy(buf, ptr, l);
        addr += l;
        buf += l;
        count -= l;
    }
    return 0;
}

static int virtio_memcpy_to_ram(VIRTIODevice *s, virtio_phys_addr_t addr, 
                                const uint8_t *buf, int count)
{
    uint8_t *ptr = {0};
    int l = {0};

    while (count > 0) {
        l = min_int(count, VIRTIO_PAGE_SIZE - (addr & (VIRTIO_PAGE_SIZE - 1)));
        ptr = s->get_ram_ptr(s, addr);
        if (!ptr)
            return -1;
        memcpy(ptr, buf, l);
        addr += l;
        buf += l;
        count -= l;
    }
    return 0;
}

static int get_desc(VIRTIODevice *s, VIRTIODesc *desc,  
                    int queue_idx, int desc_idx)
{
    QueueState *qs = &s->queue[queue_idx];
    return virtio_memcpy_from_ram(s, (void *)desc, qs->desc_addr +
                                  desc_idx * sizeof(VIRTIODesc),
                                  sizeof(VIRTIODesc));
}

static int memcpy_to_from_queue(VIRTIODevice *s, uint8_t *buf,
                                int queue_idx, int desc_idx,
                                int offset, int count, bool to_queue)
{
    VIRTIODesc desc = {0};
    int l, f_write_flag = {0};

    if (count == 0)
        return 0;

    get_desc(s, &desc, queue_idx, desc_idx);

    if (to_queue) {
        f_write_flag = VRING_DESC_F_WRITE;
        /* find the first write descriptor */
        for(;;) {
            if ((desc.flags & VRING_DESC_F_WRITE) == f_write_flag)
                break;
            if (!(desc.flags & VRING_DESC_F_NEXT))
                return -1;
            desc_idx = desc.next;
            get_desc(s, &desc, queue_idx, desc_idx);
        }
    } else {
        f_write_flag = 0;
    }

    /* find the descriptor at offset */
    for(;;) {
        if ((desc.flags & VRING_DESC_F_WRITE) != f_write_flag)
            return -1;
        if (offset < desc.len)
            break;
        if (!(desc.flags & VRING_DESC_F_NEXT))
            return -1;
        desc_idx = desc.next;
        offset -= desc.len;
        get_desc(s, &desc, queue_idx, desc_idx);
    }

    for(;;) {
        l = min_int(count, desc.len - offset);
        if (to_queue)
            virtio_memcpy_to_ram(s, desc.addr + offset, buf, l);
        else
            virtio_memcpy_from_ram(s, buf, desc.addr + offset, l);
        count -= l;
        if (count == 0)
            break;
        offset += l;
        buf += l;
        if (offset == desc.len) {
            if (!(desc.flags & VRING_DESC_F_NEXT))
                return -1;
            desc_idx = desc.next;
            get_desc(s, &desc, queue_idx, desc_idx);
            if ((desc.flags & VRING_DESC_F_WRITE) != f_write_flag)
                return -1;
            offset = 0;
        }
    }
    return 0;
}

static int memcpy_from_queue(VIRTIODevice *s, void *buf,
                             int queue_idx, int desc_idx,
                             int offset, int count)
{
    return memcpy_to_from_queue(s, buf, queue_idx, desc_idx, offset, count,
                                false);
}

static int memcpy_to_queue(VIRTIODevice *s,
                           int queue_idx, int desc_idx,
                           int offset, const void *buf, int count)
{
    return memcpy_to_from_queue(s, (void *)buf, queue_idx, desc_idx, offset,
                                count, true);
}

static void set_irq(IRQSignal *irq, int level) {
    // TODO
}

/* signal that the descriptor has been consumed */
static void virtio_consume_desc(VIRTIODevice *s,
                                int queue_idx, int desc_idx, int desc_len)
{
    QueueState *qs = &s->queue[queue_idx];
    virtio_phys_addr_t addr = {0};
    uint32_t index = {0};

    addr = qs->used_addr + 2;
    index = virtio_read16(s, addr);
    virtio_write16(s, addr, index + 1);

    addr = qs->used_addr + 4 + (index & (qs->num - 1)) * 8;
    virtio_write32(s, addr, desc_idx);
    virtio_write32(s, addr + 4, desc_len);

    s->int_status |= 1;
    set_irq(s->irq, 1);
}

static int get_desc_rw_size(VIRTIODevice *s, 
                             int *pread_size, int *pwrite_size,
                             int queue_idx, int desc_idx)
{
    VIRTIODesc desc = {0};
    int read_size, write_size = {0};

    read_size = 0;
    write_size = 0;
    get_desc(s, &desc, queue_idx, desc_idx);

    for(;;) {
        if (desc.flags & VRING_DESC_F_WRITE)
            break;
        read_size += desc.len;
        if (!(desc.flags & VRING_DESC_F_NEXT))
            goto done;
        desc_idx = desc.next;
        get_desc(s, &desc, queue_idx, desc_idx);
    }
    
    for(;;) {
        if (!(desc.flags & VRING_DESC_F_WRITE))
            return -1;
        write_size += desc.len;
        if (!(desc.flags & VRING_DESC_F_NEXT))
            break;
        desc_idx = desc.next;
        get_desc(s, &desc, queue_idx, desc_idx);
    }

 done:
    *pread_size = read_size;
    *pwrite_size = write_size;
    return 0;
}

/* XXX: test if the queue is ready ? */
static void queue_notify(VIRTIODevice *s, int queue_idx)
{
    QueueState *qs = &s->queue[queue_idx];
    uint16_t avail_idx = {0};
    int desc_idx = {0}, read_size = {0}, write_size = {0};

    if (qs->manual_recv)
        return;

    avail_idx = virtio_read16(s, qs->avail_addr + 2);
    while (qs->last_avail_idx != avail_idx) {
        desc_idx = virtio_read16(s, qs->avail_addr + 4 + 
                                 (qs->last_avail_idx & (qs->num - 1)) * 2);
        if (!get_desc_rw_size(s, &read_size, &write_size, queue_idx, desc_idx)) {
#ifdef DEBUG_VIRTIO
            if (s->debug & VIRTIO_DEBUG_IO) {
                printf("queue_notify: idx=%d read_size=%d write_size=%d\n",
                       queue_idx, read_size, write_size);
            }
#endif
            if (s->device_recv(s, queue_idx, desc_idx,
                               read_size, write_size) < 0)
                break;
        }
        qs->last_avail_idx++;
    }
}

static uint32_t virtio_config_read(VIRTIODevice *s, uint32_t offset,
                                   int size_log2)
{
    uint32_t val = {0};
    switch(size_log2) {
    case 0:
        if (offset < s->config_space_size) {
            val = s->config_space[offset];
        } else {
            val = 0;
        }
        break;
    case 1:
        if (offset < (s->config_space_size - 1)) {
            val = get_le16(&s->config_space[offset]);
        } else {
            val = 0;
        }
        break;
    case 2:
        if (offset < (s->config_space_size - 3)) {
            val = get_le32(s->config_space + offset);
        } else {
            val = 0;
        }
        break;
    default:
        abort();
    }
    return val;
}

static void virtio_config_write(VIRTIODevice *s, uint32_t offset,
                                uint32_t val, int size_log2)
{
    switch(size_log2) {
    case 0:
        if (offset < s->config_space_size) {
            s->config_space[offset] = val;
            if (s->config_write)
                s->config_write(s);
        }
        break;
    case 1:
        if (offset < s->config_space_size - 1) {
            put_le16(s->config_space + offset, val);
            if (s->config_write)
                s->config_write(s);
        }
        break;
    case 2:
        if (offset < s->config_space_size - 3) {
            put_le32(s->config_space + offset, val);
            if (s->config_write)
                s->config_write(s);
        }
        break;
    }
}

uint32_t virtio_mmio_read(VIRTIODevice *s, uint32_t offset, int size_log2)
{
    
    uint32_t val = {0};

    pthread_mutex_lock(&s->lock);
    if (offset >= VIRTIO_MMIO_CONFIG) {
        val = virtio_config_read(s, offset - VIRTIO_MMIO_CONFIG, size_log2);
        goto end;
    }

    if (size_log2 == 2) {
        switch(offset) {
        case VIRTIO_MMIO_MAGIC_VALUE:
            val = 0x74726976;
            break;
        case VIRTIO_MMIO_VERSION:
            val = 2;
            break;
        case VIRTIO_MMIO_DEVICE_ID:
            val = s->device_id;
            break;
        case VIRTIO_MMIO_VENDOR_ID:
            val = s->vendor_id;
            break;
        case VIRTIO_MMIO_DEVICE_FEATURES:
            switch(s->device_features_sel) {
            case 0:
                val = s->device_features;
                break;
            case 1:
                val = 1; /* version 1 */
                break;
            default:
                val = 0;
                break;
            }
            break;
        case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
            val = s->device_features_sel;
            break;
        case VIRTIO_MMIO_QUEUE_SEL:
            val = s->queue_sel;
            break;
        case VIRTIO_MMIO_QUEUE_NUM_MAX:
            val = MAX_QUEUE_NUM;
            break;
        case VIRTIO_MMIO_QUEUE_NUM:
            val = s->queue[s->queue_sel].num;
            break;
        case VIRTIO_MMIO_QUEUE_DESC_LOW:
            val = s->queue[s->queue_sel].desc_addr;
            break;
        case VIRTIO_MMIO_QUEUE_AVAIL_LOW:
            val = s->queue[s->queue_sel].avail_addr;
            break;
        case VIRTIO_MMIO_QUEUE_USED_LOW:
            val = s->queue[s->queue_sel].used_addr;
            break;
        case VIRTIO_MMIO_QUEUE_DESC_HIGH:
            val = s->queue[s->queue_sel].desc_addr >> 32;
            break;
        case VIRTIO_MMIO_QUEUE_AVAIL_HIGH:
            val = s->queue[s->queue_sel].avail_addr >> 32;
            break;
        case VIRTIO_MMIO_QUEUE_USED_HIGH:
            val = s->queue[s->queue_sel].used_addr >> 32;
            break;
        case VIRTIO_MMIO_QUEUE_READY:
            val = s->queue[s->queue_sel].ready;
            break;
        case VIRTIO_MMIO_INTERRUPT_STATUS:
            val = s->int_status;
            break;
        case VIRTIO_MMIO_STATUS:
            val = s->status;
            break;
        case VIRTIO_MMIO_CONFIG_GENERATION:
            val = 0;
            break;
        default:
            val = 0;
            break;
        }
    } else {
        val = 0;
    }
#ifdef DEBUG_VIRTIO
    if (s->debug & VIRTIO_DEBUG_IO) {
        printf("virto_mmio_read: offset=0x%x val=0x%x size=%d\n", 
               offset, val, 1 << size_log2);
    }
#endif
    
end:
    pthread_mutex_unlock(&s->lock);
    return val;
}

static void set_low32(virtio_phys_addr_t *paddr, uint32_t val)
{
    *paddr = (*paddr & ~(virtio_phys_addr_t)0xffffffff) | val;
}

static void set_high32(virtio_phys_addr_t *paddr, uint32_t val)
{
    *paddr = (*paddr & 0xffffffff) | ((virtio_phys_addr_t)val << 32);
}

void virtio_mmio_write(VIRTIODevice *s, uint32_t offset,
                       uint32_t val, int size_log2)
{
    pthread_mutex_lock(&s->lock);
#ifdef DEBUG_VIRTIO
    if (s->debug & VIRTIO_DEBUG_IO) {
        printf("virto_mmio_write: offset=0x%x val=0x%x size=%d\n",
               offset, val, 1 << size_log2);
    }
#endif
    if (offset >= VIRTIO_MMIO_CONFIG) {
        virtio_config_write(s, offset - VIRTIO_MMIO_CONFIG, val, size_log2);
        goto end;
    }

    if (size_log2 == 2) {
        switch(offset) {
        case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
            s->device_features_sel = val;
            break;
        case VIRTIO_MMIO_QUEUE_SEL:
            if (val < MAX_QUEUE)
                s->queue_sel = val;
            break;
        case VIRTIO_MMIO_QUEUE_NUM:
            if ((val & (val - 1)) == 0 && val > 0) {
                s->queue[s->queue_sel].num = val;
            }
            break;
        case VIRTIO_MMIO_QUEUE_DESC_LOW:
            set_low32(&s->queue[s->queue_sel].desc_addr, val);
            break;
        case VIRTIO_MMIO_QUEUE_AVAIL_LOW:
            set_low32(&s->queue[s->queue_sel].avail_addr, val);
            break;
        case VIRTIO_MMIO_QUEUE_USED_LOW:
            set_low32(&s->queue[s->queue_sel].used_addr, val);
            break;
        case VIRTIO_MMIO_QUEUE_DESC_HIGH:
            set_high32(&s->queue[s->queue_sel].desc_addr, val);
            break;
        case VIRTIO_MMIO_QUEUE_AVAIL_HIGH:
            set_high32(&s->queue[s->queue_sel].avail_addr, val);
            break;
        case VIRTIO_MMIO_QUEUE_USED_HIGH:
            set_high32(&s->queue[s->queue_sel].used_addr, val);
            break;
        case VIRTIO_MMIO_STATUS:
            s->status = val;
            if (val == 0) {
                /* reset */
                set_irq(s->irq, 0);
                virtio_reset(s);
            }
            break;
        case VIRTIO_MMIO_QUEUE_READY:
            s->queue[s->queue_sel].ready = val & 1;
            break;
        case VIRTIO_MMIO_QUEUE_NOTIFY:
            if (val < MAX_QUEUE)
                queue_notify(s, val);
            break;
        case VIRTIO_MMIO_INTERRUPT_ACK:
            s->int_status &= ~val;
            if (s->int_status == 0) {
                set_irq(s->irq, 0);
            }
            break;
        }
    }
end:
    pthread_mutex_unlock(&s->lock);
}

void virtio_set_debug(VIRTIODevice *s, int debug)
{
    s->debug = debug;
}

void virtio_config_change_notify(VIRTIODevice *s)
{
    /* INT_CONFIG interrupt */
    s->int_status |= 2;
    set_irq(s->irq, 1);
}

/*********************************************************************/
/* block device */

typedef struct {
    uint32_t type;
    uint8_t *buf;
    int write_size;
    int queue_idx;
    int desc_idx;
} BlockRequest;

typedef struct VIRTIOBlockDevice {
    VIRTIODevice common;
    BlockDevice *bs;

    bool req_in_progress;
    BlockRequest req; /* request in progress */
} VIRTIOBlockDevice;

typedef struct {
    uint32_t type;
    uint32_t ioprio;
    uint64_t sector_num;
} BlockRequestHeader;

#define VIRTIO_BLK_T_IN          0
#define VIRTIO_BLK_T_OUT         1
#define VIRTIO_BLK_T_FLUSH       4
#define VIRTIO_BLK_T_FLUSH_OUT   5

#define VIRTIO_BLK_S_OK     0
#define VIRTIO_BLK_S_IOERR  1
#define VIRTIO_BLK_S_UNSUPP 2

#define SECTOR_SIZE 512

static void virtio_block_req_end(VIRTIODevice *s, int ret)
{
    VIRTIOBlockDevice *s1 = (VIRTIOBlockDevice *)s;
    int write_size = {0};
    int queue_idx = s1->req.queue_idx;
    int desc_idx = s1->req.desc_idx;
    uint8_t *buf = {0}, buf1[1] = {0};

    switch(s1->req.type) {
    case VIRTIO_BLK_T_IN:
        write_size = s1->req.write_size;
        buf = s1->req.buf;
        if (ret < 0) {
            buf[write_size - 1] = VIRTIO_BLK_S_IOERR;
        } else {
            buf[write_size - 1] = VIRTIO_BLK_S_OK;
        }
        memcpy_to_queue(s, queue_idx, desc_idx, 0, buf, write_size);
        free(buf);
        virtio_consume_desc(s, queue_idx, desc_idx, write_size);
        break;
    case VIRTIO_BLK_T_OUT:
        if (ret < 0)
            buf1[0] = VIRTIO_BLK_S_IOERR;
        else
            buf1[0] = VIRTIO_BLK_S_OK;
        memcpy_to_queue(s, queue_idx, desc_idx, 0, buf1, sizeof(buf1));
        virtio_consume_desc(s, queue_idx, desc_idx, 1);
        break;
    default:
        abort();
    }
}

static void virtio_block_req_cb(void *opaque, int ret)
{
    VIRTIODevice *s = opaque;
    VIRTIOBlockDevice *s1 = (VIRTIOBlockDevice *)s;
    pthread_mutex_lock(&s->lock);

    virtio_block_req_end(s, ret);
    
    s1->req_in_progress = false;

    /* handle next requests */
    queue_notify((VIRTIODevice *)s, s1->req.queue_idx);
    pthread_mutex_unlock(&s->lock);
}

/* XXX: handle async I/O */
static int virtio_block_recv_request(VIRTIODevice *s, int queue_idx,
                                     int desc_idx, int read_size,
                                     int write_size)
{
    VIRTIOBlockDevice *s1 = (VIRTIOBlockDevice *)s;
    BlockDevice *bs = s1->bs;
    BlockRequestHeader h = {0};
    uint8_t *buf = {0};
    int len, ret = {0};

    if (s1->req_in_progress)
        return -1;
    
    if (memcpy_from_queue(s, &h, queue_idx, desc_idx, 0, sizeof(h)) < 0)
        return 0;
    s1->req.type = h.type;
    s1->req.queue_idx = queue_idx;
    s1->req.desc_idx = desc_idx;
    switch(h.type) {
    case VIRTIO_BLK_T_IN:
        s1->req.buf = malloc(write_size);
        memset(s1->req.buf, 0, write_size);
        s1->req.write_size = write_size;
        ret = bs->read_async(bs, h.sector_num, s1->req.buf, 
                             (write_size - 1) / SECTOR_SIZE,
                             virtio_block_req_cb, s);
        if (ret > 0) {
            /* asyncronous read */
            s1->req_in_progress = true;
        } else {
            virtio_block_req_end(s, ret);
        }
        break;
    case VIRTIO_BLK_T_OUT:
        assert(write_size >= 1);
        len = read_size - sizeof(h);
        buf = malloc(len);
        memset(buf, 0, len);
        memcpy_from_queue(s, buf, queue_idx, desc_idx, sizeof(h), len);
        ret = bs->write_async(bs, h.sector_num, buf, len / SECTOR_SIZE,
                              virtio_block_req_cb, s);
        free(buf);
        if (ret > 0) {
            /* asyncronous write */
            s1->req_in_progress = true;
        } else {
            virtio_block_req_end(s, ret);
        }
        break;
    default:
        break;
    }
    return 0;
}

VIRTIODevice *virtio_block_init(VIRTIOBusDef *bus, BlockDevice *bs)
{
    VIRTIOBlockDevice *s = {0};
    uint64_t nb_sectors = {0};

    s = malloc(sizeof(*s));
    *s = (VIRTIOBlockDevice){0};
    virtio_init(&s->common, bus,
                2, 8, virtio_block_recv_request);
    s->bs = bs;
    
    nb_sectors = bs->get_sector_count(bs);
    put_le32(s->common.config_space, nb_sectors);
    put_le32(s->common.config_space + 4, nb_sectors >> 32);

    return (VIRTIODevice *)s;
}

/*********************************************************************/
/* network device */

typedef struct VIRTIONetDevice {
    VIRTIODevice common;
    EthernetDevice *es;
    int header_size;
} VIRTIONetDevice;

typedef struct {
    uint8_t flags;
    uint8_t gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;
} VIRTIONetHeader;

static int virtio_net_recv_request(VIRTIODevice *s, int queue_idx,
                                   int desc_idx, int read_size,
                                   int write_size)
{
    VIRTIONetDevice *s1 = (VIRTIONetDevice *)s;
    EthernetDevice *es = s1->es;
    VIRTIONetHeader h = {0};
    uint8_t *buf = {0};
    int len = {0};

    if (queue_idx == 1) {
        /* send to network */
        if (memcpy_from_queue(s, &h, queue_idx, desc_idx, 0, s1->header_size) < 0)
            return 0;
        len = read_size - s1->header_size;
        buf = malloc(len);
        memset(buf, 0, len);
        memcpy_from_queue(s, buf, queue_idx, desc_idx, s1->header_size, len);
        es->write_packet_to_ether(es, buf, len);
        free(buf);
        virtio_consume_desc(s, queue_idx, desc_idx, 0);
    }
    return 0;
}

static bool virtio_net_can_write_packet(EthernetDevice *es)
{
    VIRTIODevice *s = es->device_opaque;
    QueueState *qs = &s->queue[0];
    uint16_t avail_idx = {0};

    if (!qs->ready)
        return false;
    avail_idx = virtio_read16(s, qs->avail_addr + 2);
    return qs->last_avail_idx != avail_idx;
}

static void virtio_net_write_packet(EthernetDevice *es, const uint8_t *buf, int buf_len)
{
    VIRTIODevice *s = es->device_opaque;
    VIRTIONetDevice *s1 = (VIRTIONetDevice *)s;
    int queue_idx = 0;
    QueueState *qs = &s->queue[queue_idx];
    int desc_idx = {0};
    VIRTIONetHeader h = {0};
    int len = {0}, read_size = {0}, write_size = {0};
    uint16_t avail_idx = {0};

    if (!qs->ready)
        return;
    avail_idx = virtio_read16(s, qs->avail_addr + 2);
    if (qs->last_avail_idx == avail_idx)
        return;
    desc_idx = virtio_read16(s, qs->avail_addr + 4 + 
                             (qs->last_avail_idx & (qs->num - 1)) * 2);
    if (get_desc_rw_size(s, &read_size, &write_size, queue_idx, desc_idx))
        return;
    len = s1->header_size + buf_len; 
    if (len > write_size)
        return;
    memset(&h, 0, s1->header_size);
    memcpy_to_queue(s, queue_idx, desc_idx, 0, &h, s1->header_size);
    memcpy_to_queue(s, queue_idx, desc_idx, s1->header_size, buf, buf_len);
    virtio_consume_desc(s, queue_idx, desc_idx, len);
    qs->last_avail_idx++;
}

VIRTIODevice *virtio_net_init(VIRTIOBusDef *bus, EthernetDevice *es)
{
    VIRTIONetDevice *s = NULL;

    s = malloc(sizeof(*s));
    *s = (VIRTIONetDevice){0};
    virtio_init(&s->common, bus,
                1, 6 + 2, virtio_net_recv_request);
    /* VIRTIO_NET_F_MAC, VIRTIO_NET_F_STATUS */
    s->common.device_features = (1 << 5) /* | (1 << 16) */;
    s->common.queue[0].manual_recv = true;
    s->es = es;
    memcpy(s->common.config_space, es->mac_addr, 6);
    /* status */
    s->common.config_space[6] = 0;
    s->common.config_space[7] = 0;

    s->header_size = sizeof(VIRTIONetHeader);
    
    es->device_opaque = s;
    es->can_write_packet_to_virtio = virtio_net_can_write_packet;
    es->write_packet_to_virtio = virtio_net_write_packet;
    return (VIRTIODevice *)s;
}
