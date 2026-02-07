#pragma once

#define VIRTIO_BLK_MMIO_ADDR (1024LL * 1024 * 1024 * 1024)
#define VIRTIO_BLK_IRQ 10
#define VIRTIO_BLK_CMDLINE " virtio_mmio.device=4K@0x10000000000:10"

#define VIRTIO_NET_MMIO_ADDR (1025LL * 1024 * 1024 * 1024)
#define VIRTIO_NET_IRQ 11
#define VIRTIO_NET_CMDLINE " virtio_mmio.device=4K@0x10040000000:11"

#define DEFAULT_KERNEL_CMDLINE "console=ttyS0 debug"

#define VIRTIO_PAGE_SIZE 4096
#define SECTOR_SIZE 512
#define TAP_BUF_SIZE 4096

#define MVVMM_DEBUG 1

#define DEBUG(...) do { \
    if (MVVMM_DEBUG) { \
        fprintf(stderr, __VA_ARGS__); \
    } \
} while (0);
