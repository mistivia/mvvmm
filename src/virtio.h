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
#include <pthread.h>

namespace mvvmm {

typedef uint64_t virtio_phys_addr_t;

class irq_signal {
public:
    irq_signal() {}
    ~irq_signal();
    irq_signal(const irq_signal&) = delete;
    irq_signal& operator=(const irq_signal&) = delete;
    irq_signal(irq_signal&&); 
    irq_signal& operator=(irq_signal&&);
    int init(int vm_fd, int irq_line);
    void set_irq(int level);
    void trigger();
private:
    void swap(irq_signal& other) noexcept;
    int m_vmfd = -1;
    int m_irqline = -1;
    int m_irqfd = -1;
};

struct virtio_bus_def {
    explicit virtio_bus_def() = default;
    struct guest_mem_map *mem_map = nullptr;
    int vmfd = -1;
    int irqline = -1;
};

struct virtio_device;

uint32_t virtio_mmio_read(virtio_device *s, uint32_t offset1, int size);
void virtio_mmio_write(virtio_device *s, uint32_t offset,
                       uint32_t val, int size);

/* block device */
struct blk_io_callback_arg;
typedef void block_device_comp_func(struct blk_io_callback_arg *callback_arg, int ret);

struct disk_image {
    explicit disk_image() = default;
    int fd = -1;
    uint64_t size = 0;
};

struct block_device;

struct block_request {
    explicit block_request() = default;
    uint32_t type = 0;
    uint8_t *buf = nullptr;
    int write_size = 0;
    int queue_idx = 0;
    int desc_idx = 0;
} ;

struct blk_io_callback_arg {
    explicit blk_io_callback_arg() = default;
    virtio_device *s = nullptr;
    block_request req{};
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

constexpr static int VIRTIO_MAX_QUEUE = 2;
constexpr static int VIRTIO_MAX_CONFIG_SPACE_SIZE = 256;

struct QueueState {
    explicit QueueState() = default;
    uint32_t ready = 0; /* 0 or 1 */
    uint32_t num = 0;
    uint16_t last_avail_idx = 0;
    virtio_phys_addr_t desc_addr = {0};
    virtio_phys_addr_t avail_addr = {0};
    virtio_phys_addr_t used_addr = {0};
    bool manual_recv = {0}; /* if true, the device_recv() callback is not called */
};


struct VIRTIODesc{
    explicit VIRTIODesc() = default;
    uint64_t addr = 0;
    uint32_t len = 0;
    uint16_t flags = 0; /* VRING_DESC_F_x */
    uint16_t next = 0;
};

/* return < 0 to stop the notification (it must be manually restarted
   later), 0 if OK */
typedef int virtio_deviceRecvFunc(virtio_device *s1, int queue_idx, int desc_idx, int read_size,
                                  int write_size);

/* return NULL if no RAM at this address. The mapping is valid for one page */
typedef uint8_t *VIRTIOGetRAMPtrFunc(virtio_device *s, virtio_phys_addr_t paddr);

struct virtio_device {
    explicit virtio_device() = default;
    struct guest_mem_map *mem_map = nullptr;
    /* MMIO only */
    irq_signal irq{};
    int vmfd = -1;

    uint32_t int_status = 0;
    uint32_t status = 0;
    uint32_t device_features_sel = 0;
    uint32_t queue_sel = 0; /* currently selected queue */
    QueueState queue[VIRTIO_MAX_QUEUE];

    /* device specific */
    uint32_t device_id = 0;
    uint32_t vendor_id = 0;
    uint32_t device_features = 0;
    virtio_deviceRecvFunc *device_recv = nullptr;
    uint32_t config_space_size = 0; /* in bytes, must be multiple of 4 */
    uint8_t config_space[VIRTIO_MAX_CONFIG_SPACE_SIZE] = {0};
    pthread_mutex_t lock = {0};
    int max_queue_num = 0;
    uint64_t mmio_addr = 0;         /* MMIO base address */
    int ioeventfd[VIRTIO_MAX_QUEUE] = {0};   /* eventfd for each queue notify */
    pthread_t ioeventfd_thread = 0; /* thread polling ioeventfds */
    bool ioeventfd_enabled = 0;     /* whether ioeventfd is active */
};

struct virtio_block_device {
    explicit virtio_block_device() = default;
    enum class cmd_type : uint32_t {
        in = 0,
        out = 1,
        flush = 4,
        flush_out = 5,
    };

    enum class result_type {
        ok = 0,
        io_err = 1,
        unsupported = 2,
    };
    virtio_device common{};
    block_device *bs = nullptr;
};

typedef struct virtio_net_device {
    explicit virtio_net_device() = default;
    virtio_device common{};
    ethernet_device *es = nullptr;
    int header_size = 0;
} virtio_net_device;

} // namespace mvvmm
