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


#include <cstring>
#include <cstdlib>
#include <cstdio>

#include <memory>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/if.h>
#include <linux/if_tun.h>

#include <thread>
#include <mutex>

#include "mvvm.h"
#include "netdev.h"
#include "virtio.h"
#include "config.h"

namespace mvvmm {

static void write_packet_to_ether(ethernet_device *net, const uint8_t *buf, int len)
{
    if (!net->ctx || net->ctx->fd < 0 || !buf || len <= 0) {
        return;
    }
    write(net->ctx->fd, buf, len);
}

static ssize_t timed_read(int fd, void *buf, size_t len, int timeout_ms)
{
    pollfd pfd = {0};
    int ret = 0;

    pfd.fd = fd;
    pfd.events = POLLIN;

    ret = poll(&pfd, 1, timeout_ms);
    if (ret < 0) {
        return -1;
    }
    if (ret == 0) {
        // Timeout
        errno = ETIMEDOUT;
        return -1;
    }

    if (pfd.revents & POLLIN) {
        return read(fd, buf, len);
    }

    // Error or hangup
    errno = EIO;
    return -1;
}

static void tap_net_rx_thread(std::shared_ptr<ethernet_device> net)
{
    uint8_t buf[TAP_BUF_SIZE] = {0};

    if (!net->ctx || net->ctx->fd < 0) {
        return;
    }

    while (1) {
        ssize_t len = timed_read(net->ctx->fd, buf, sizeof(buf), 300);
        if (len < 0) {
            if (errno == EINTR || errno == ETIMEDOUT) {
                std::unique_lock<std::mutex> lk{net->ctx->lock};
                if (net->ctx->quit) {
                    return;
                }
                continue;
            }
            perror("tap_net_rx_thread: timed_read failed");
            break;
        }

        if (len == 0) {
            // TAP device closed
            break;
        }

        // Check if virtio net device can receive packet
        if (net->can_write_packet_to_virtio && net->can_write_packet_to_virtio(net.get())) {
            net->write_packet_to_virtio(net.get(), buf, len);
        }
        // If virtio queue is full, packet is dropped
    }
}

// Initialize virtio network device with TAP backend
int mvvm_init_virtio_net(mvvm *self, const char *tap_ifname)
{
    virtio_bus_def bus;
    ifreq ifr = {0};

    // Allocate TAP device context
    auto ctx = std::make_unique<tap_net_ctx>();
    ctx->quit = 0;
    // Open TUN/TAP clone device
    ctx->fd = open("/dev/net/tun", O_RDWR | O_NONBLOCK);
    if (ctx->fd < 0) {
        perror("mvvm_init_virtio_net: open /dev/net/tun failed");
        return -1;
    }

    // Configure device as TAP (Ethernet layer) without packet info header
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    if (tap_ifname) {
        strncpy(ifr.ifr_name, tap_ifname, IFNAMSIZ - 1);
        ifr.ifr_name[IFNAMSIZ - 1] = '\0';
    }

    if (ioctl(ctx->fd, TUNSETIFF, &ifr) < 0) {
        perror("mvvm_init_virtio_net: ioctl TUNSETIFF failed");
        return -1;
    }

    // Store the actual interface name assigned by kernel
    strncpy(ctx->ifname, ifr.ifr_name, IFNAMSIZ - 1);
    ctx->ifname[IFNAMSIZ - 1] = '\0';

    // Allocate and initialize ethernet_device structure
    auto net = std::make_shared<ethernet_device>();

    // Set locally administered MAC address (52:54:00:12:34:56)
    // In production, this should be configurable or derived from TAP
    net->mac_addr[0] = 0x52;
    net->mac_addr[1] = 0x54;
    net->mac_addr[2] = 0x00;
    net->mac_addr[3] = 0x12;
    net->mac_addr[4] = 0x34;
    net->mac_addr[5] = 0x56;

    net->write_packet_to_ether = write_packet_to_ether;
    net->ctx = std::move(ctx);

    bus.vmfd = self->m_vm_fd;
    bus.irqline = VIRTIO_NET_IRQ;

    // Setup virtio bus definition
    bus.mem_map = self->m_mem_map;

    // Initialize virtio network device
    self->m_net = virtio_net_init(bus, VIRTIO_NET_MMIO_ADDR, net);
    if (!self->m_net) {
        fprintf(stderr, "failed to initialize virtio net device\n");
        return -1;
    }
    // Start RX thread to handle incoming packets from TAP
    try {
        net->ctx->rx_thread = std::thread{[net](){
            tap_net_rx_thread(net);
        }};
    } catch (...) {
        fprintf(stderr, "failed to create TAP RX thread\n");
        return -1;
    }
    return 0;
}

void mvvm_destroy_virtio_net(mvvm *self)
{
    virtio_net_destroy(self->m_net);
    delete self->m_net;
}

} // namespace mvvmm
