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

#include "threadpool.h"
#include <cstdint>

#include <memory>
#include <thread>
#include <mutex>
#include <functional>

#include <linux/if.h>

namespace mvvmm {

using virtio_phys_addr_t = uint64_t;

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
using block_device_comp_func = void (*)(std::unique_ptr<blk_io_callback_arg> callback_arg, int ret);

struct disk_image {
    explicit disk_image() = default;
    int fd = -1;
    uint64_t size = 0;
};

struct block_request {
    explicit block_request() = default;
    uint32_t type = 0;
    std::unique_ptr<uint8_t[]> buf;
    int write_size = 0;
    int queue_idx = 0;
    int desc_idx = 0;
} ;

struct blk_io_callback_arg {
    explicit blk_io_callback_arg() = default;
    virtio_device *s = nullptr;
    block_request req{};
};

class block_device {
public:
    explicit block_device() = default;
    virtual ~block_device() {}

    virtual int64_t get_sector_count() = 0;
    virtual void read_async(uint64_t sector_num, int n, // n is sector number
                            block_device_comp_func cb, std::unique_ptr<blk_io_callback_arg> cbarg) = 0;
    virtual void write_async(uint64_t sector_num, int n, // n is sector nubmer
                             block_device_comp_func cb, std::unique_ptr<blk_io_callback_arg> cbarg) = 0;
};

std::unique_ptr<virtio_device> virtio_block_init(virtio_bus_def bus, uint64_t mmio_addr, std::shared_ptr<block_device> bs);
void virtio_block_req_end(std::unique_ptr<blk_io_callback_arg> arg, int ret);

class ethernet_device {
public:
    explicit ethernet_device() = default;
    virtual ~ethernet_device() {}
    virtual void write_packet_to_ether(const uint8_t *buf, int len) = 0;
    bool can_write_packet_to_virtio();
    void write_packet_to_virtio(const uint8_t *buf, int len);
protected:
    uint8_t m_mac_addr[6] = {0}; /* mac address of the interface */
    virtio_device *m_owner = nullptr;
    
    friend std::unique_ptr<virtio_device> virtio_net_init(virtio_bus_def bus, uint64_t mmio_addr, std::shared_ptr<ethernet_device> es);
};

std::unique_ptr<virtio_device> virtio_net_init(virtio_bus_def bus, uint64_t mmio_addr, std::shared_ptr<ethernet_device> es);

constexpr static int VIRTIO_MAX_QUEUE = 2;
constexpr static int VIRTIO_MAX_CONFIG_SPACE_SIZE = 256;

struct queue_state {
    explicit queue_state() = default;
    uint32_t ready = 0; /* 0 or 1 */
    uint32_t num = 0;
    uint16_t last_avail_idx = 0;
    virtio_phys_addr_t desc_addr = {0};
    virtio_phys_addr_t avail_addr = {0};
    virtio_phys_addr_t used_addr = {0};
    bool manual_recv = {0}; /* if true, the device_recv() callback is not called */
};


struct virtio_desc{
    explicit virtio_desc() = default;
    uint64_t addr = 0;
    uint32_t len = 0;
    uint16_t flags = 0; /* VRING_DESC_F_x */
    uint16_t next = 0;
};

// return < 0 to stop the notification (it must be manually restarted
// later), 0 if OK
using virtio_devoce_recv_fn =
    std::function<int(virtio_device *s1,
                      int queue_idx,
                      int desc_idx,
                      int read_size,
                      int write_size)>;

struct virtio_device {
    explicit virtio_device() = default;
    virtual ~virtio_device();
    struct guest_mem_map *mem_map = nullptr;
    /* MMIO only */
    irq_signal irq{};
    int vmfd = -1;
    uint32_t int_status = 0;
    uint32_t status = 0;
    uint32_t device_features_sel = 0;
    uint32_t queue_sel = 0; /* currently selected queue */
    queue_state queue[VIRTIO_MAX_QUEUE];

    /* device specific */
    uint32_t device_id = 0;
    uint32_t vendor_id = 0;
    uint32_t device_features = 0;
    virtio_devoce_recv_fn device_recv = nullptr;
    uint32_t config_space_size = 0; /* in bytes, must be multiple of 4 */
    uint8_t config_space[VIRTIO_MAX_CONFIG_SPACE_SIZE] = {0};
    std::mutex lock{};
    int max_queue_num = 0;
    uint64_t mmio_addr = 0;         /* MMIO base address */
    int ioeventfd[VIRTIO_MAX_QUEUE] = {0};   /* eventfd for each queue notify */
    std::thread ioeventfd_thread{}; /* thread polling ioeventfds */
    bool ioeventfd_enabled = 0;     /* whether ioeventfd is active */
};

struct virtio_block_device : public virtio_device {
    explicit virtio_block_device() = default;
    virtual ~virtio_block_device() override;
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
    std::shared_ptr<block_device> bs{};
};

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

struct virtio_net_device: public virtio_device {
    explicit virtio_net_device() = default;
    virtual ~virtio_net_device() override;
    int header_size = sizeof(virtio_net_header);
    std::shared_ptr<ethernet_device> es;
};

} // namespace mvvmm
