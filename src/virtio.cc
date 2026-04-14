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

#include <cstdint>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>
#include <stdarg.h>
#include <atomic>
#include <linux/kvm.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <poll.h>
#include <errno.h>

#include "virtio.h"
#include "config.h"
#include "mvvm.h"

namespace mvvmm {

/* MMIO addresses - from the Linux kernel */
constexpr static inline int VIRTIO_MMIO_MAGIC_VALUE = 0x000;
constexpr static inline int VIRTIO_MMIO_VERSION = 0x004;
constexpr static inline int VIRTIO_MMIO_DEVICE_ID = 0x008;
constexpr static inline int VIRTIO_MMIO_VENDOR_ID = 0x00c;
constexpr static inline int VIRTIO_MMIO_DEVICE_FEATURES = 0x010;
constexpr static inline int VIRTIO_MMIO_DEVICE_FEATURES_SEL = 0x014;
constexpr static inline int VIRTIO_MMIO_DRIVER_FEATURES = 0x020;
constexpr static inline int VIRTIO_MMIO_DRIVER_FEATURES_SEL = 0x024;
constexpr static inline int VIRTIO_MMIO_QUEUE_SEL = 0x030;
constexpr static inline int VIRTIO_MMIO_QUEUE_NUM_MAX = 0x034;
constexpr static inline int VIRTIO_MMIO_QUEUE_NUM = 0x038;
constexpr static inline int VIRTIO_MMIO_QUEUE_READY = 0x044;
constexpr static inline int VIRTIO_MMIO_QUEUE_NOTIFY = 0x050;
constexpr static inline int VIRTIO_MMIO_INTERRUPT_STATUS = 0x060;
constexpr static inline int VIRTIO_MMIO_INTERRUPT_ACK = 0x064;
constexpr static inline int VIRTIO_MMIO_STATUS = 0x070;
constexpr static inline int VIRTIO_MMIO_QUEUE_DESC_LOW = 0x080;
constexpr static inline int VIRTIO_MMIO_QUEUE_DESC_HIGH = 0x084;
constexpr static inline int VIRTIO_MMIO_QUEUE_AVAIL_LOW = 0x090;
constexpr static inline int VIRTIO_MMIO_QUEUE_AVAIL_HIGH = 0x094;
constexpr static inline int VIRTIO_MMIO_QUEUE_USED_LOW = 0x0a0;
constexpr static inline int VIRTIO_MMIO_QUEUE_USED_HIGH = 0x0a4;
constexpr static inline int VIRTIO_MMIO_CONFIG_GENERATION = 0x0fc;
constexpr static inline int VIRTIO_MMIO_CONFIG = 0x100;

constexpr static inline int VRING_DESC_F_NEXT = 1;
constexpr static inline int VRING_DESC_F_WRITE = 2;
constexpr static inline int VRING_DESC_F_INDIRECT = 4;


static void queue_notify(virtio_device *s, int queue_idx);
static int virtio_ioeventfd_start(virtio_device *s);
static void virtio_ioeventfd_stop(virtio_device *s);

static void put_le32(void *ptr, uint32_t val)
{
    if (ptr == NULL) {
        return;
    }
    uint8_t *const p = (uint8_t *const)ptr;
    p[0] = (uint8_t)(val & 0xFF);
    p[1] = (uint8_t)((val >> 8) & 0xFF);
    p[2] = (uint8_t)((val >> 16) & 0xFF);
    p[3] = (uint8_t)((val >> 24) & 0xFF);
}

static void put_le16(void *ptr, uint16_t val)
{
    if (ptr == NULL) {
        return;
    }
    uint8_t *const p = (uint8_t *const)ptr;
    p[0] = (uint8_t)(val & 0xFF);
    p[1] = (uint8_t)((val >> 8) & 0xFF);
}

static uint32_t get_le32(void *ptr)
{
    if (ptr == NULL) {
        return 0;
    }
    const uint8_t *const p = (const uint8_t *const)ptr;
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t get_le16(void *ptr)
{
    if (ptr == NULL) {
        return 0;
    }
    const uint8_t *const p = (const uint8_t *const)ptr;
    return ((uint16_t)p[0]) | ((uint16_t)p[1] << 8);
}

static void virtio_reset(virtio_device *s)
{
    int i = 0;

    s->status = 0;
    s->queue_sel = 0;
    s->device_features_sel = 0;
    s->int_status = 0;
    for (i = 0; i < VIRTIO_MAX_QUEUE; i++) {
        QueueState *qs = &s->queue[i];
        qs->avail_addr = 0;
        qs->desc_addr = 0;
        qs->used_addr = 0;
        qs->last_avail_idx = 0;
        qs->ready = 0;
        qs->num = s->max_queue_num;
    }
}

static uint8_t *guest_addr_to_host_addr(virtio_device *s, uint64_t guest_addr)
{
    struct guest_mem_map *mem_map = s->mem_map;
    if (guest_addr > mem_map->size) {
        return NULL;
    }
    return (uint8_t *)mem_map->host_mem + guest_addr;
}

static int virtio_init(virtio_device *s, virtio_bus_def bus, uint64_t mmio_addr, uint32_t device_id,
                       int config_space_size, virtio_deviceRecvFunc *device_recv, int max_queue_num)
{
    s->mem_map = bus.mem_map;
    s->vmfd = bus.vmfd;
    s->mmio_addr = mmio_addr;
    for (int i = 0; i < VIRTIO_MAX_QUEUE; i++) {
        s->ioeventfd[i] = -1;
    }
    s->ioeventfd_enabled = false;

    s->device_id = device_id;
    s->vendor_id = 0xffff;
    s->config_space_size = config_space_size;
    s->device_recv = device_recv;
    s->max_queue_num = max_queue_num;
    pthread_mutex_init(&s->lock, NULL);
    virtio_reset(s);

    /* Initialize irqfd for this device */
    if (s->irq.init(bus.vmfd, bus.irqline)) {
        fprintf(stderr, "virtio_init: failed to initialize irqfd\n");
        return -1;
    }
    /* Initialize ioeventfd for queue notifications */
    if (virtio_ioeventfd_start(s) < 0) {
        fprintf(stderr,
                "virtio_init: ioeventfd initialization failed, falling back to MMIO exits\n");
        return -1;
    }
    return 0;
}

static uint16_t virtio_read16(virtio_device *s, virtio_phys_addr_t addr)
{
    std::atomic_thread_fence(std::memory_order_acquire);
    uint8_t *ptr = NULL;
    if (addr & 1)
        return 0; /* unaligned access are not supported */
    ptr = guest_addr_to_host_addr(s, addr);
    if (!ptr)
        return 0;
    return *(uint16_t *)ptr;
}

static void virtio_write16(virtio_device *s, virtio_phys_addr_t addr, uint16_t val)
{
    uint8_t *ptr = NULL;
    if (addr & 1)
        return; /* unaligned access are not supported */
    ptr = guest_addr_to_host_addr(s, addr);
    if (!ptr)
        return;
    *(uint16_t *)ptr = val;
    std::atomic_thread_fence(std::memory_order_release);
}

static void virtio_write32(virtio_device *s, virtio_phys_addr_t addr, uint32_t val)
{
    uint8_t *ptr = NULL;
    if (addr & 3)
        return; /* unaligned access are not supported */
    ptr = guest_addr_to_host_addr(s, addr);
    if (!ptr)
        return;
    *(uint32_t *)ptr = val;
    std::atomic_thread_fence(std::memory_order_release);
}

static inline int min_int(int a, int b) { return a < b ? a : b; }

static int virtio_memcpy_from_guest(virtio_device *s, uint8_t *buf, virtio_phys_addr_t addr,
                                    int count)
{
    uint8_t *ptr = NULL;
    int l = {0};

    while (count > 0) {
        l = min_int(count, VIRTIO_PAGE_SIZE - (addr & (VIRTIO_PAGE_SIZE - 1)));
        ptr = guest_addr_to_host_addr(s, addr);
        if (!ptr)
            return -1;
        std::atomic_thread_fence(std::memory_order_acquire);
        memcpy(buf, ptr, l);
        addr += l;
        buf += l;
        count -= l;
    }
    return 0;
}

static int virtio_memcpy_to_guest(virtio_device *s, virtio_phys_addr_t addr, const uint8_t *buf,
                                  int count)
{
    uint8_t *ptr = {0};
    int l = {0};

    while (count > 0) {
        l = min_int(count, VIRTIO_PAGE_SIZE - (addr & (VIRTIO_PAGE_SIZE - 1)));
        ptr = guest_addr_to_host_addr(s, addr);
        if (!ptr)
            return -1;
        memcpy(ptr, buf, l);
        std::atomic_thread_fence(std::memory_order_release);
        addr += l;
        buf += l;
        count -= l;
    }
    return 0;
}

static int get_desc(virtio_device *s, VIRTIODesc *desc, int queue_idx, int desc_idx)
{
    QueueState *qs = &s->queue[queue_idx];
    return virtio_memcpy_from_guest(
        s, (uint8_t *)desc, qs->desc_addr + desc_idx * sizeof(VIRTIODesc), sizeof(VIRTIODesc));
}

static int memcpy_to_from_queue(virtio_device *s, uint8_t *buf, int queue_idx, int desc_idx,
                                int offset, int count, bool to_queue)
{
    VIRTIODesc desc{};
    int l, f_write_flag = {0};

    if (count == 0)
        return 0;

    if (get_desc(s, &desc, queue_idx, desc_idx) < 0) {
        fprintf(stderr, "memcpy_to_from_queue get_desc failed.\n");
        return -1;
    }

    if (to_queue) {
        f_write_flag = VRING_DESC_F_WRITE;
        /* find the first write descriptor */
        for (int i = 0;; i++) {
            if (i > 1024) {
                fprintf(stderr, "memcpy_to_from_queue infinite loop1.\n");
                return -1;
            }
            if ((desc.flags & VRING_DESC_F_WRITE) == f_write_flag)
                break;
            if (!(desc.flags & VRING_DESC_F_NEXT))
                return -1;
            desc_idx = desc.next;
            if (get_desc(s, &desc, queue_idx, desc_idx) < 0) {
                fprintf(stderr, "memcpy_to_from_queue: failed to get_desc2.\n");
                return -1;
            }
        }
    } else {
        f_write_flag = 0;
    }

    /* find the descriptor at offset */
    for (int i = 0;; i++) {
        if (i > 1024) {
            fprintf(stderr, "memcpy_to_from_queue infinite loop2.\n");
            return -1;
        }
        if ((desc.flags & VRING_DESC_F_WRITE) != f_write_flag)
            return -1;
        if (offset < (int)desc.len)
            break;
        if (!(desc.flags & VRING_DESC_F_NEXT))
            return -1;
        desc_idx = desc.next;
        offset -= desc.len;
        if (get_desc(s, &desc, queue_idx, desc_idx) < 0) {
            fprintf(stderr, "memcpy_to_from_queue: failed to get_desc3.\n");
            return -1;
        }
    }

    for (int i = 0;; i++) {
        if (i > 1024) {
            fprintf(stderr, "memcpy_to_from_queue infinite loop1.\n");
            return -1;
        }
        l = min_int(count, desc.len - offset);
        if (to_queue)
            virtio_memcpy_to_guest(s, desc.addr + offset, buf, l);
        else
            virtio_memcpy_from_guest(s, buf, desc.addr + offset, l);
        count -= l;
        if (count == 0)
            break;
        offset += l;
        buf += l;
        if (offset == (int)desc.len) {
            if (!(desc.flags & VRING_DESC_F_NEXT))
                return -1;
            desc_idx = desc.next;
            if (get_desc(s, &desc, queue_idx, desc_idx) < 0) {
                fprintf(stderr, "memcpy_to_from_queue: failed to get_desc4.\n");
                return -1;
            }
            if ((desc.flags & VRING_DESC_F_WRITE) != f_write_flag)
                return -1;
            offset = 0;
        }
    }
    return 0;
}

static int memcpy_from_queue(virtio_device *s, void *buf, int queue_idx, int desc_idx, int offset,
                             int count)
{
    return memcpy_to_from_queue(s, (uint8_t *)buf, queue_idx, desc_idx, offset, count, false);
}

static int memcpy_to_queue(virtio_device *s, int queue_idx, int desc_idx, int offset,
                           const void *buf, int count)
{
    return memcpy_to_from_queue(s, (uint8_t *)buf, queue_idx, desc_idx, offset, count, true);
}

void irq_signal::set_irq(int level)
{
    struct kvm_irq_level irq = {0};
    irq.irq = m_irqline;
    irq.level = level;
    ioctl(m_vmfd, KVM_IRQ_LINE, &irq);
}

void irq_signal::trigger()
{
    uint64_t val = 1;
    if (m_irqfd >= 0) {
        write(m_irqfd, &val, sizeof(val));
    }
}

int irq_signal::init(int vmfd, int irqline)
{
    int fd;
    struct kvm_irqfd irqfd = {0};
    m_vmfd = vmfd;
    m_irqline = irqline;
    fd = eventfd(0, EFD_NONBLOCK);
    if (fd < 0) {
        perror("eventfd");
        return -1;
    }

    irqfd.fd = fd;
    irqfd.gsi = irqline;
    irqfd.flags = 0;

    if (ioctl(vmfd, KVM_IRQFD, &irqfd) < 0) {
        perror("KVM_IRQFD");
        close(fd);
        return -1;
    }
    m_irqfd = fd;
    return 0;
}

irq_signal::~irq_signal()
{
    if (m_irqfd >= 0) {
        close(m_irqfd);
        m_irqfd = -1;
    }
}

void irq_signal::swap(irq_signal& other) noexcept {
    std::swap(m_irqfd, other.m_irqfd);
    std::swap(m_irqline, other.m_irqline);
    std::swap(m_vmfd, other.m_vmfd);
}

irq_signal::irq_signal(irq_signal&& from)
{
    this->swap(from);
}

irq_signal& irq_signal::operator=(irq_signal&& from)
{
    irq_signal temp{std::move(from)};
    this->swap(temp);
    return *this;
}

/* ioeventfd functions */
static int virtio_ioeventfd_register(virtio_device *s, int queue_idx)
{
    struct kvm_ioeventfd ioevent = {0};
    int fd;

    fd = eventfd(0, EFD_NONBLOCK);
    if (fd < 0) {
        perror("eventfd");
        return -1;
    }

    ioevent.fd = fd;
    ioevent.datamatch = queue_idx; /* guest writes queue index to QUEUE_NOTIFY */
    ioevent.len = 4;               /* 32-bit write */
    ioevent.addr = s->mmio_addr + VIRTIO_MMIO_QUEUE_NOTIFY;
    ioevent.flags = KVM_IOEVENTFD_FLAG_DATAMATCH;

    if (ioctl(s->vmfd, KVM_IOEVENTFD, &ioevent) < 0) {
        perror("KVM_IOEVENTFD");
        close(fd);
        return -1;
    }

    s->ioeventfd[queue_idx] = fd;
    return 0;
}

static void virtio_ioeventfd_unregister(virtio_device *s, int queue_idx)
{
    int fd = s->ioeventfd[queue_idx];
    if (fd >= 0) {
        close(fd);
        s->ioeventfd[queue_idx] = -1;
    }
}

static void *virtio_ioeventfd_poll_thread(void *arg)
{
    virtio_device *s = (virtio_device *)arg;
    struct pollfd pfds[VIRTIO_MAX_QUEUE];
    int nfds = 0;
    int i;

    /* setup pollfd for each queue */
    for (i = 0; i < VIRTIO_MAX_QUEUE; i++) {
        if (s->ioeventfd[i] >= 0) {
            pfds[nfds].fd = s->ioeventfd[i];
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }
    }

    while (s->ioeventfd_enabled) {
        int ret = poll(pfds, nfds, 300); /* timeout 1 second */
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }
        if (ret == 0) {
            /* timeout, check flag again */
            continue;
        }

        for (i = 0; i < nfds; i++) {
            if (pfds[i].revents & POLLIN) {
                uint64_t val;
                /* read to clear eventfd */
                read(pfds[i].fd, &val, sizeof(val));
                /* find queue index */
                int qidx;
                for (qidx = 0; qidx < VIRTIO_MAX_QUEUE; qidx++) {
                    if (s->ioeventfd[qidx] == pfds[i].fd)
                        break;
                }
                if (qidx < VIRTIO_MAX_QUEUE) {
                    pthread_mutex_lock(&s->lock);
                    queue_notify(s, qidx);
                    pthread_mutex_unlock(&s->lock);
                }
            }
        }
    }
    return NULL;
}

static int virtio_ioeventfd_start(virtio_device *s)
{
    int i;
    for (i = 0; i < VIRTIO_MAX_QUEUE; i++) {
        if (virtio_ioeventfd_register(s, i) < 0) {
            /* cleanup previously registered */
            while (--i >= 0) {
                virtio_ioeventfd_unregister(s, i);
            }
            return -1;
        }
    }
    s->ioeventfd_enabled = true;
    if (pthread_create(&s->ioeventfd_thread, NULL, virtio_ioeventfd_poll_thread, s) != 0) {
        perror("pthread_create");
        s->ioeventfd_enabled = false;
        for (i = 0; i < VIRTIO_MAX_QUEUE; i++) {
            virtio_ioeventfd_unregister(s, i);
        }
        return -1;
    }
    return 0;
}

static void virtio_ioeventfd_stop(virtio_device *s)
{
    s->ioeventfd_enabled = false;
    pthread_join(s->ioeventfd_thread, NULL);
    for (int i = 0; i < VIRTIO_MAX_QUEUE; i++) {
        virtio_ioeventfd_unregister(s, i);
    }
}

/* signal that the descriptor has been consumed */
static void virtio_consume_desc(virtio_device *s, int queue_idx, int desc_idx, int desc_len)
{
    QueueState *qs = &s->queue[queue_idx];
    virtio_phys_addr_t index_addr = 0, ring_addr = {0};
    uint32_t index = {0};

    index_addr = qs->used_addr + 2;
    index = virtio_read16(s, index_addr);
    ring_addr = qs->used_addr + 4 + (index & (qs->num - 1)) * 8;
    virtio_write32(s, ring_addr, desc_idx);
    virtio_write32(s, ring_addr + 4, desc_len);
    virtio_write16(s, index_addr, index + 1);

    uint16_t flags = virtio_read16(s, qs->avail_addr);
    if (flags & 0x01) { // intr suppression
        return;
    }
    s->int_status |= 1;
    s->irq.trigger();
}

static int get_desc_rw_size(virtio_device *s, int *pread_size, int *pwrite_size, int queue_idx,
                            int desc_idx)
{
    VIRTIODesc desc{};
    int read_size, write_size = {0};

    read_size = 0;
    write_size = 0;
    get_desc(s, &desc, queue_idx, desc_idx);

    for (;;) {
        if (desc.flags & VRING_DESC_F_WRITE)
            break;
        read_size += desc.len;
        if (!(desc.flags & VRING_DESC_F_NEXT))
            goto done;
        desc_idx = desc.next;
        get_desc(s, &desc, queue_idx, desc_idx);
    }

    for (;;) {
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
static void queue_notify(virtio_device *s, int queue_idx)
{
    QueueState *qs = &s->queue[queue_idx];
    uint16_t avail_idx = {0};
    int desc_idx = {0}, read_size = {0}, write_size = {0};

    if (qs->manual_recv)
        return;

    avail_idx = virtio_read16(s, qs->avail_addr + 2);
    while (qs->last_avail_idx != avail_idx) {
        desc_idx = virtio_read16(s, qs->avail_addr + 4 + (qs->last_avail_idx & (qs->num - 1)) * 2);
        if (!get_desc_rw_size(s, &read_size, &write_size, queue_idx, desc_idx)) {
            if (s->device_recv(s, queue_idx, desc_idx, read_size, write_size) < 0)
                break;
        }
        qs->last_avail_idx++;
    }
}

static uint32_t virtio_config_read(virtio_device *s, uint32_t offset, int size)
{
    uint32_t val = {0};
    switch (size) {
    case 1:
        if (offset < s->config_space_size) {
            val = s->config_space[offset];
        } else {
            val = 0;
        }
        break;
    case 2:
        if (offset < (s->config_space_size - 1)) {
            val = get_le16(&s->config_space[offset]);
        } else {
            val = 0;
        }
        break;
    case 4:
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

static void virtio_config_write(virtio_device *s, uint32_t offset, uint32_t val, int size)
{
    switch (size) {
    case 1:
        if (offset < s->config_space_size) {
            s->config_space[offset] = val;
        }
        break;
    case 2:
        if (offset < s->config_space_size - 1) {
            put_le16(s->config_space + offset, val);
        }
        break;
    case 4:
        if (offset < s->config_space_size - 3) {
            put_le32(s->config_space + offset, val);
        }
        break;
    }
}

uint32_t virtio_mmio_read(virtio_device *s, uint32_t offset, int size)
{

    uint32_t val = {0};

    pthread_mutex_lock(&s->lock);
    if (offset >= VIRTIO_MMIO_CONFIG) {
        val = virtio_config_read(s, offset - VIRTIO_MMIO_CONFIG, size);
        goto end;
    }

    if (size == 4) {
        switch (offset) {
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
            switch (s->device_features_sel) {
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
            val = s->max_queue_num;
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
        fprintf(stderr, "virtio mmio read error: len != 4\n");
        val = 0;
    }

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

void virtio_mmio_write(virtio_device *s, uint32_t offset, uint32_t val, int size)
{
    pthread_mutex_lock(&s->lock);
    if (offset >= VIRTIO_MMIO_CONFIG) {
        virtio_config_write(s, offset - VIRTIO_MMIO_CONFIG, val, size);
        goto end;
    }

    if (size == 4) {
        switch (offset) {
        case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
            s->device_features_sel = val;
            break;
        case VIRTIO_MMIO_QUEUE_SEL:
            if (val < VIRTIO_MAX_QUEUE)
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
                s->int_status = 0;
                s->irq.set_irq(0);
                virtio_reset(s);
            }
            break;
        case VIRTIO_MMIO_QUEUE_READY:
            s->queue[s->queue_sel].ready = val & 1;
            break;
        case VIRTIO_MMIO_QUEUE_NOTIFY:
            if (val < VIRTIO_MAX_QUEUE)
                queue_notify(s, val);
            break;
        case VIRTIO_MMIO_INTERRUPT_ACK:
            s->int_status &= ~val;
            break;
        }
    } else {
        fprintf(stderr, "virtio mmio write error: len != 4\n");
    }
end:
    pthread_mutex_unlock(&s->lock);
}

/*********************************************************************/
/* block device */

typedef struct {
    uint32_t type;
    uint32_t ioprio;
    uint64_t sector_num;
} block_request_header;

static void virtio_block_req_end(struct blk_io_callback_arg *arg, int ret)
{
    virtio_device *s = arg->s;
    int write_size = {0};
    int queue_idx = arg->req.queue_idx;
    int desc_idx = arg->req.desc_idx;
    uint8_t *buf = {0}, buf1[1] = {0};

    switch (arg->req.type) {
    case (uint32_t)virtio_block_device::cmd_type::in:
        write_size = arg->req.write_size;
        buf = arg->req.buf;
        if (ret < 0) {
            buf[write_size - 1] = (int)virtio_block_device::result_type::io_err;
        } else {
            buf[write_size - 1] = (int)virtio_block_device::result_type::ok;
        }
        memcpy_to_queue(s, queue_idx, desc_idx, 0, buf, write_size);
        free(buf);
        virtio_consume_desc(s, queue_idx, desc_idx, write_size);
        break;
    case (uint32_t)virtio_block_device::cmd_type::out:
        if (ret < 0)
            buf1[0] = (int)virtio_block_device::result_type::io_err;
        else
            buf1[0] = (int)virtio_block_device::result_type::ok;
        memcpy_to_queue(s, queue_idx, desc_idx, 0, buf1, sizeof(buf1));
        virtio_consume_desc(s, queue_idx, desc_idx, 1);
        break;
    default:
        abort();
    }
}

static void virtio_block_req_cb(struct blk_io_callback_arg *arg, int ret)
{
    virtio_device *s = arg->s;
    pthread_mutex_lock(&s->lock);

    virtio_block_req_end(arg, ret);

    /* handle next requests */
    queue_notify((virtio_device *)s, arg->req.queue_idx);
    free(arg);
    pthread_mutex_unlock(&s->lock);
}

static int virtio_block_recv_request(virtio_device *s, int queue_idx, int desc_idx, int read_size,
                                     int write_size)
{
    virtio_block_device *s1 = (virtio_block_device *)s;
    block_device *bs = s1->bs;
    block_request_header h = {0};
    uint8_t *buf = {0};
    int len, ret = {0};

    if (memcpy_from_queue(s, &h, queue_idx, desc_idx, 0, sizeof(h)) < 0)
        return 0;
    auto iocb_arg = new blk_io_callback_arg{};
    iocb_arg->s = s;
    iocb_arg->req.type = h.type;
    iocb_arg->req.queue_idx = queue_idx;
    iocb_arg->req.desc_idx = desc_idx;
    switch (h.type) {
    case (uint32_t)virtio_block_device::cmd_type::in:
        iocb_arg->req.buf = new uint8_t[write_size];
        memset(iocb_arg->req.buf, 0, write_size);
        iocb_arg->req.write_size = write_size;
        ret = bs->read_async(bs, h.sector_num, iocb_arg->req.buf, (write_size - 1) / SECTOR_SIZE,
                             virtio_block_req_cb, iocb_arg);
        if (ret < 0) {
            virtio_block_req_end(iocb_arg, ret);
            delete iocb_arg;
        }
        break;
    case (uint32_t)virtio_block_device::cmd_type::out:
        if (write_size < 1) {
            fprintf(stderr, "virtio_block_recv_request invalid write_size.\n");
            abort();
        }
        len = read_size - sizeof(h);
        if (len <= 0) {
            fprintf(stderr, "virtio_block_recv_request invalid read_size.\n");
            abort();
        }
        buf = new uint8_t[len];
        memset(buf, 0, len);
        memcpy_from_queue(s, buf, queue_idx, desc_idx, sizeof(h), len);
        ret = bs->write_async(bs, h.sector_num, buf, len / SECTOR_SIZE, virtio_block_req_cb,
                              iocb_arg);
        if (ret < 0) {
            delete[] buf;
            virtio_block_req_end(iocb_arg, ret);
            delete iocb_arg;
        }
        break;
    default:
        break;
    }
    return 0;
}

virtio_device *virtio_block_init(virtio_bus_def bus, uint64_t mmio_addr, block_device *bs)
{
    virtio_block_device *s = {0};
    uint64_t nb_sectors = {0};
    s = new virtio_block_device();
    if (virtio_init(&s->common, std::move(bus), mmio_addr, 2, 8, virtio_block_recv_request,
                    VIRTIO_BLK_MAX_QUEUE_NUM) < 0) {
        delete s;
        return NULL;
    }
    s->bs = bs;

    nb_sectors = bs->get_sector_count(bs);
    put_le32(s->common.config_space, nb_sectors);
    put_le32(s->common.config_space + 4, nb_sectors >> 32);

    return (virtio_device *)s;
}

void virtio_block_destroy(virtio_device *s)
{
    virtio_block_device *bs = (virtio_block_device *)s;
    virtio_ioeventfd_stop(s);
    s->irq.~irq_signal();
    free(bs->bs);
}

void *virtio_block_get_opaque(virtio_device *s)
{
    virtio_block_device *bs = (virtio_block_device *)s;
    return bs->bs->opaque;
}

/*********************************************************************/
/* network device */
struct __attribute__((packed))
virtio_net_header {
    uint8_t flags;
    uint8_t gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;
};

static int virtio_net_recv_request(virtio_device *s, int queue_idx, int desc_idx, int read_size,
                                   int write_size)
{
    virtio_net_device *s1 = (virtio_net_device *)s;
    ethernet_device *es = s1->es;
    virtio_net_header h = {0};
    uint8_t *buf = {0};
    int len = {0};
    if (memcpy_from_queue(s, &h, queue_idx, desc_idx, 0, s1->header_size) < 0)
        return 0;
    len = read_size - s1->header_size;
    buf = new uint8_t[len];
    memset(buf, 0, len);
    memcpy_from_queue(s, buf, queue_idx, desc_idx, s1->header_size, len);
    es->write_packet_to_ether(es, buf, len);
    delete[] buf;
    virtio_consume_desc(s, queue_idx, desc_idx, 0);
    return 0;
}

static bool virtio_net_can_write_packet(ethernet_device *es)
{
    bool ret = 0;
    virtio_device *s = (virtio_device *)es->device_opaque;
    pthread_mutex_lock(&s->lock);
    QueueState *qs = &s->queue[0];
    uint16_t avail_idx = {0};

    if (!qs->ready) {
        ret = false;
        goto end;
    }
    avail_idx = virtio_read16(s, qs->avail_addr + 2);
    ret = qs->last_avail_idx != avail_idx;
end:
    pthread_mutex_unlock(&s->lock);
    return ret;
}

static void virtio_net_write_packet(ethernet_device *es, const uint8_t *buf, int buf_len)
{
    virtio_device *s = (virtio_device *)es->device_opaque;
    pthread_mutex_lock(&s->lock);

    virtio_net_device *s1 = (virtio_net_device *)s;
    int queue_idx = 0;
    QueueState *qs = &s->queue[queue_idx];
    int desc_idx = {0};
    virtio_net_header h = {0};
    int len = {0}, read_size = {0}, write_size = {0};
    uint16_t avail_idx = {0};

    if (!qs->ready)
        goto end;
    avail_idx = virtio_read16(s, qs->avail_addr + 2);
    if (qs->last_avail_idx == avail_idx)
        goto end;
    desc_idx = virtio_read16(s, qs->avail_addr + 4 + (qs->last_avail_idx & (qs->num - 1)) * 2);
    if (get_desc_rw_size(s, &read_size, &write_size, queue_idx, desc_idx))
        goto end;
    len = s1->header_size + buf_len;
    if (len > write_size)
        goto end;
    memset(&h, 0, s1->header_size);
    memcpy_to_queue(s, queue_idx, desc_idx, 0, &h, s1->header_size);
    memcpy_to_queue(s, queue_idx, desc_idx, s1->header_size, buf, buf_len);
    virtio_consume_desc(s, queue_idx, desc_idx, len);
    qs->last_avail_idx++;
end:
    pthread_mutex_unlock(&s->lock);
}

virtio_device *virtio_net_init(virtio_bus_def bus, uint64_t mmio_addr, ethernet_device *es)
{
    virtio_net_device *s = NULL;
    s = new virtio_net_device();
    if (virtio_init(&s->common, std::move(bus), mmio_addr, 1, 6 + 2, virtio_net_recv_request,
                    VIRTIO_NET_MAX_QUEUE_NUM) < 0) {
        delete s;
        return NULL;
    }
    /* VIRTIO_NET_F_MAC, VIRTIO_NET_F_STATUS */
    s->common.device_features = (1 << 5) /* | (1 << 16) */;
    s->common.queue[0].manual_recv = true;
    s->es = es;
    memcpy(s->common.config_space, es->mac_addr, 6);
    /* status */
    s->common.config_space[6] = 0;
    s->common.config_space[7] = 0;

    s->header_size = sizeof(virtio_net_header);

    es->device_opaque = s;
    es->can_write_packet_to_virtio = virtio_net_can_write_packet;
    es->write_packet_to_virtio = virtio_net_write_packet;
    return (virtio_device *)s;
}

void virtio_net_destroy(virtio_device *s)
{
    virtio_net_device *es = (virtio_net_device *)s;
    virtio_ioeventfd_stop(s);
    s->irq.~irq_signal();
    free(es->es);
}

void *virtio_net_get_opaque(virtio_device *s)
{
    virtio_net_device *es = (virtio_net_device *)s;
    return es->es->opaque;
}

} // namespace mvvmm
