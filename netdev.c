#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/if.h>
#include <linux/if_tun.h>

#include "mvvm.h"
#include "netdev.h"
#include "virtio.h"
#include "config.h"

struct tap_net_ctx {
    int fd;
    char ifname[IFNAMSIZ];
    pthread_t rx_thread;
    int quit;
    pthread_mutex_t lock;
};

static void
write_packet_to_ether(EthernetDevice *net, const uint8_t *buf, int len)
{
    struct tap_net_ctx *ctx = net->opaque;
    if (!ctx || ctx->fd < 0 || !buf || len <= 0) {
        return;
    }
    write(ctx->fd, buf, len);
}

static void *
tap_net_rx_thread(void *arg)
{
    EthernetDevice *net = arg;
    struct tap_net_ctx *ctx = net->opaque;
    uint8_t buf[TAP_BUF_SIZE];
    struct pollfd pfd;
    int ret;

    if (!ctx || ctx->fd < 0) {
        return NULL;
    }

    pfd.fd = ctx->fd;
    pfd.events = POLLIN;

    while (1) {
        ret = poll(&pfd, 1, 300);
        struct tap_net_ctx *ctx = net->opaque;
        pthread_mutex_lock(&ctx->lock);
        if (ctx->quit) {
            pthread_mutex_unlock(&ctx->lock);
            return NULL;
        }
        pthread_mutex_unlock(&ctx->lock);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("tap_net_rx_thread: poll failed");
            break;
        }

        if (pfd.revents & POLLIN) {
            while (1) {
                ssize_t len = read(ctx->fd, buf, sizeof(buf));
                if (len < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    perror("tap_net_rx_thread: read failed");
                    goto end;
                }
                if (len == 0) {
                    // TAP device closed
                    goto end;
                }

                // Check if virtio net device can receive packet
                if (net->can_write_packet_to_virtio &&
                    net->can_write_packet_to_virtio(net)) {
                    net->write_packet_to_virtio(net, buf, len);
                }
                // If virtio queue is full, packet is dropped
            }
        }
    }
end:
    return NULL;
}

// Initialize virtio network device with TAP backend
int
mvvm_init_virtio_net(struct mvvm *self, const char *tap_ifname)
{
    struct tap_net_ctx *ctx = NULL;
    EthernetDevice *net = NULL;
    struct IRQSignal irq = {0};
    VIRTIOBusDef bus = {0};
    struct ifreq ifr = {0};
    int ret = -1;

    // Allocate TAP device context
    ctx = malloc(sizeof(*ctx));
    if (!ctx) {
        fprintf(stderr, "failed to allocate TAP network context\n");
        return -1;
    }
    ctx->quit = 0;
    pthread_mutex_init(&ctx->lock, NULL);
    // Open TUN/TAP clone device
    ctx->fd = open("/dev/net/tun", O_RDWR | O_NONBLOCK);
    if (ctx->fd < 0) {
        perror("mvvm_init_virtio_net: open /dev/net/tun failed");
        goto fail;
    }

    // Configure device as TAP (Ethernet layer) without packet info header
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    if (tap_ifname) {
        strncpy(ifr.ifr_name, tap_ifname, IFNAMSIZ - 1);
        ifr.ifr_name[IFNAMSIZ - 1] = '\0';
    }

    if (ioctl(ctx->fd, TUNSETIFF, &ifr) < 0) {
        perror("mvvm_init_virtio_net: ioctl TUNSETIFF failed");
        goto fail;
    }

    // Store the actual interface name assigned by kernel
    strncpy(ctx->ifname, ifr.ifr_name, IFNAMSIZ - 1);
    ctx->ifname[IFNAMSIZ - 1] = '\0';

    // Allocate and initialize EthernetDevice structure
    net = malloc(sizeof(*net));
    if (!net) {
        fprintf(stderr, "failed to allocate EthernetDevice structure\n");
        goto fail;
    }

    // Set locally administered MAC address (52:54:00:12:34:56)
    // In production, this should be configurable or derived from TAP
    net->mac_addr[0] = 0x52;
    net->mac_addr[1] = 0x54;
    net->mac_addr[2] = 0x00;
    net->mac_addr[3] = 0x12;
    net->mac_addr[4] = 0x34;
    net->mac_addr[5] = 0x56;

    net->write_packet_to_ether = write_packet_to_ether;
    net->opaque = ctx;

    irq.vmfd = self->vm_fd;
    irq.irqline = VIRTIO_NET_IRQ;

    // Setup virtio bus definition
    bus.mem_map = self->mem_map;
    bus.irq = irq;

    // Initialize virtio network device
    self->net = virtio_net_init(bus, net);
    if (!self->net) {
        fprintf(stderr, "failed to initialize virtio net device\n");
        goto fail;
    }
    // Start RX thread to handle incoming packets from TAP
    pthread_t rx_thread;
    if (pthread_create(&rx_thread, NULL, tap_net_rx_thread, net) != 0) {
        fprintf(stderr, "failed to create TAP RX thread\n");
        goto fail;
    }
    ctx->rx_thread = rx_thread;
    return 0;

fail:
    if (net) {
        free(net);
    }
    if (ctx) {
        if (ctx->fd >= 0) {
            close(ctx->fd);
        }
        free(ctx);
    }
    return ret;
}

void mvvm_destroy_virtio_net(struct mvvm *self) {
    struct tap_net_ctx *ctx = virtio_net_get_opaque(self->net);
    pthread_mutex_lock(&ctx->lock);
    ctx->quit = 1;
    pthread_mutex_unlock(&ctx->lock);
    void *ret = NULL;
    pthread_join(ctx->rx_thread, &ret);
    free(ctx);
    virtio_net_destroy(self->net);
    free(self->net);
}