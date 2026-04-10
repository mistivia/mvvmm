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

#include "mvvm.h"

#include <errno.h>
#include <memory>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/kvm.h>
#include <linux/kvm_para.h>
#include <asm/bootparam.h>

#include "blkdev.h"
#include "netdev.h"
#include "config.h"
#include "serial.h"
#include "virtio.h"

namespace mvvmm {

void mvvm::set_flat_mode(struct kvm_segment *seg)
{
    seg->base = 0;
    seg->limit = 0xffffffff;
    seg->g = 1;
    seg->db = 1;
}

int mvvm::init_cpu(int kvm_fd, int cpu_fd)
{
    struct kvm_sregs sregs = {0};
    struct kvm_regs regs = {0};
    struct kvm_cpuid2 *cpuid = NULL;
    int max_entries = 100;

    if (ioctl(cpu_fd, KVM_GET_SREGS, &(sregs)) < 0) {
        fprintf(stderr, "failed to get sregs.\n");
        return -1;
    }
    if (ioctl(cpu_fd, KVM_GET_REGS, &(regs)) < 0) {
        fprintf(stderr, "failed to get regs.\n");
        return -1;
    }
    set_flat_mode(&sregs.cs);
    set_flat_mode(&sregs.ds);
    set_flat_mode(&sregs.es);
    set_flat_mode(&sregs.fs);
    set_flat_mode(&sregs.gs);
    set_flat_mode(&sregs.ss);
    sregs.cr0 |= 0x1;

    regs.rip = 0x100000;
    regs.rsi = 0x10000;

    if (ioctl(cpu_fd, KVM_SET_REGS, &regs) < 0) {
        fprintf(stderr, "failed to set regs.\n");
        return -1;
    }

    if (ioctl(cpu_fd, KVM_SET_SREGS, &sregs) < 0) {
        fprintf(stderr, "failed to set sregs.\n");
        return -1;
    }
    cpuid =
        (struct kvm_cpuid2 *)malloc(sizeof(*cpuid) + max_entries * sizeof(struct kvm_cpuid_entry2));
    cpuid->nent = max_entries;
    if (ioctl(kvm_fd, KVM_GET_SUPPORTED_CPUID, cpuid) < 0) {
        fprintf(stderr, "failed to get supported cpuid.\n");
        free(cpuid);
        return -1;
    }
    if (ioctl(cpu_fd, KVM_SET_CPUID2, cpuid) < 0) {
        fprintf(stderr, "failed to set cpuid.\n");
        free(cpuid);
        return -1;
    }
    free(cpuid);
    return 0;
}

#define PAGE_SIZE 4096
#define TSS_PAGES 3
#define IDENTITY_MAP_PAGES 1
#define RESERVED_PAGES (TSS_PAGES + IDENTITY_MAP_PAGES)
// Reserved pages are placed below 4GB
#define RESERVED_ADDR 0xFFFBD000ULL
#define RESERVED_SIZE (RESERVED_PAGES * PAGE_SIZE)

int mvvm::init(uint64_t mem_size, const char *disk, const char *network)
{
    struct kvm_pit_config pit = {0};
    struct kvm_userspace_memory_region mem = {0};
    uint64_t tss_addr = RESERVED_ADDR;
    uint64_t identity_map_addr = RESERVED_ADDR + TSS_PAGES * PAGE_SIZE;
    m_quit = 0;
    // Open KVM device
    m_kvm_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (m_kvm_fd < 0) {
        fprintf(stderr, "failed to open /dev/kvm\n");
        return -1;
    }
    // Create virtual machine
    m_vm_fd = ioctl(m_kvm_fd, KVM_CREATE_VM, 0);
    if (m_vm_fd < 0) {
        fprintf(stderr, "failed to create vm\n");
        return -1;
    }
    // Create IRQ chip for interrupt handling
    if (ioctl(m_vm_fd, KVM_CREATE_IRQCHIP, 0) < 0) {
        fprintf(stderr, "failed to create irqchip\n");
        return -1;
    }
    // Create PIT for timer interrupts
    if (ioctl(m_vm_fd, KVM_CREATE_PIT2, &pit) < 0) {
        fprintf(stderr, "failed to create pit\n");
        return -1;
    }

    // Set TSS address (3 pages)
    if (ioctl(m_vm_fd, KVM_SET_TSS_ADDR, tss_addr) < 0) {
        fprintf(stderr, "failed to set TSS address\n");
        return -1;
    }

    // Set Identity Map address (1 page)
    if (ioctl(m_vm_fd, KVM_SET_IDENTITY_MAP_ADDR, &identity_map_addr) < 0) {
        fprintf(stderr, "failed to set identity map address\n");
        return -1;
    }

    // Allocate guest memory
    struct guest_mem_map *mem_map = (struct guest_mem_map *)malloc(sizeof(*mem_map));
    if (!mem_map) {
        fprintf(stderr, "failed to allocate PhysMemoryMap\n");
        return -1;
    }
    mem_map->host_mem = mmap(NULL, mem_size, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (mem_map->host_mem == MAP_FAILED) {
        fprintf(stderr, "failed to mmap memory\n");
        return -1;
    }
    mem_map->size = mem_size;
    m_mem_map = mem_map;

    // Register memory regions with KVM
    if (mem_size <= RESERVED_ADDR) {
        mem.slot = 0;
        mem.flags = 0;
        mem.guest_phys_addr = 0;
        mem.memory_size = mem_size;
        mem.userspace_addr = (uint64_t)mem_map->host_mem;
        if (ioctl(m_vm_fd, KVM_SET_USER_MEMORY_REGION, &mem) < 0) {
            fprintf(stderr, "failed to set user memory region\n");
            return -1;
        }
    } else {
        mem.slot = 0;
        mem.flags = 0;
        mem.guest_phys_addr = 0;
        mem.memory_size = RESERVED_ADDR;
        mem.userspace_addr = (uint64_t)mem_map->host_mem;
        if (ioctl(m_vm_fd, KVM_SET_USER_MEMORY_REGION, &mem) < 0) {
            fprintf(stderr, "failed to set user memory region 0\n");
            return -1;
        }
        uint64_t region1_start = RESERVED_ADDR + RESERVED_SIZE;
        if (mem_size > region1_start) {
            mem.slot = 1;
            mem.flags = 0;
            mem.guest_phys_addr = region1_start;
            mem.memory_size = mem_size - region1_start;
            mem.userspace_addr = (uint64_t)mem_map->host_mem + region1_start;
            if (ioctl(m_vm_fd, KVM_SET_USER_MEMORY_REGION, &mem) < 0) {
                fprintf(stderr, "failed to set user memory region 1\n");
                return -1;
            }
        }
    }
    // Create virtual CPU
    m_cpu_fd = ioctl(m_vm_fd, KVM_CREATE_VCPU, 0);
    if (m_cpu_fd < 0) {
        fprintf(stderr, "failed to create vcpu\n");
        return -1;
    }
    if (init_cpu(m_kvm_fd, m_cpu_fd) < 0) {
        fprintf(stderr, "cpu init failed.\n");
        return -1;
    }
    // Initialize serial port
    m_serial = std::make_unique<serial>(this);
    // init virtio block device
    if (disk != NULL) {
        if (mvvm_init_virtio_blk(this, disk) < 0) {
            fprintf(stderr, "mvvm init error, failed to load disk.\n");
            return -1;
        }
    }
    if (network != NULL) {
        if (mvvm_init_virtio_net(this, "vm0") < 0) {
            fprintf(stderr, "mvvm init error, failed to open tap interface.\n");
            return -1;
        }
    }
    return 0;
}

mvvm::~mvvm()
{
    munmap(m_mem_map->host_mem, m_mem_map->size);
    close(m_cpu_fd);
    close(m_vm_fd);
    close(m_kvm_fd);
    mvvm_destroy_virtio_blk(this);
    mvvm_destroy_virtio_net(this);
    free(m_mem_map);
}

static int map_file(const char *path, size_t *size, void **out)
{
    int fd = -1;
    struct stat st = {0};
    void *addr = NULL;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("open");
        return -1;
    }

    if (fstat(fd, &st) < 0) {
        perror("fstat");
        return -1;
    }

    addr = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (addr == MAP_FAILED) {
        perror("mmap");
        return -1;
    }

    close(fd);
    *size = st.st_size;
    *out = addr;
    return 0;
}

// Setup E820 memory map in boot parameters.
void mvvm::setup_e820_map(struct boot_params *zeropage)
{
    zeropage->e820_entries = 2;
    // first 640KB
    zeropage->e820_table[0].addr = 0;
    zeropage->e820_table[0].size = 0xA0000;
    zeropage->e820_table[0].type = 1;
    // > 1MB
    if (m_mem_map->size <= RESERVED_ADDR) {
        zeropage->e820_table[1].addr = 0x100000;
        zeropage->e820_table[1].size = m_mem_map->size - 0x100000;
        zeropage->e820_table[1].type = 1;
    } else {
        zeropage->e820_table[1].addr = 0x100000;
        zeropage->e820_table[1].size = RESERVED_ADDR - 0x100000;
        zeropage->e820_table[1].type = 1;
        if (m_mem_map->size > RESERVED_ADDR + RESERVED_SIZE) {
            zeropage->e820_entries = 3;
            zeropage->e820_table[2].addr = RESERVED_ADDR + RESERVED_SIZE;
            zeropage->e820_table[2].size = m_mem_map->size - (RESERVED_ADDR + RESERVED_SIZE);
            zeropage->e820_table[2].type = 1;
        }
    }
}

// Load initrd into guest memory at 512MB mark.
// initrd_path is guaranteed to exist.
int mvvm::load_initrd(struct boot_params *zeropage, const char *initrd_path)
{
    if (initrd_path == NULL) {
        zeropage->hdr.ramdisk_image = 0;
        zeropage->hdr.ramdisk_size = 0;
        return 0;
    }
    int fd = -1;
    struct stat st = {0};
    void *initrd = NULL;
    uint32_t initrd_addr = 1024ULL * 1024 * 192;

    fd = open(initrd_path, O_RDONLY);
    if (fd < 0) {
        perror("open initrd");
        return -1;
    }
    if (fstat(fd, &st) < 0) {
        perror("fstat initrd");
        return -1;
    }
    initrd = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (initrd == MAP_FAILED) {
        perror("mmap initrd");
        return -1;
    }
    if ((uint64_t)initrd_addr + st.st_size >= m_mem_map->size) {
        fprintf(stderr, "failed to load initrd.\n");
        return -1;
    }
    memcpy((uint8_t *)m_mem_map->host_mem + initrd_addr, initrd, st.st_size);
    zeropage->hdr.ramdisk_image = initrd_addr;
    zeropage->hdr.ramdisk_size = st.st_size;
    // cleanup
    munmap(initrd, st.st_size);
    close(fd);
    return 0;
}

char *cmdline_concat(char *buf, const char *new_arg)
{
    char *ret = NULL;
    size_t n1 = strnlen(buf, 2000);
    size_t n2 = strlen(new_arg);
    if (n1 + n2 >= 2000) {
        free(buf);
        return NULL;
    }
    ret = (char *)malloc(n1 + n2 + 1);
    memcpy(ret, buf, n1);
    memcpy(ret + n1, new_arg, n2);
    ret[n1 + n2] = 0;
    free(buf);
    return ret;
}

int mvvm::load_kernel(const char *kernel_path, const char *initrd_path,
                      const char *kernel_args)
{
    int ret = 0;
    void *bz_image = NULL;
    size_t bz_image_size = 0;
    uint32_t setup_size = 0;
    struct boot_params *zeropage = NULL;
    char *cmd_line = NULL;
    char *cmdline_buf = NULL;

    if (map_file(kernel_path, &bz_image_size, &bz_image) < 0) {
        bz_image = NULL;
        fprintf(stderr, "failed to load kernel.\n");
        ret = -1;
        goto end;
    }
    if (bz_image_size >= 190 * 1024 * 1024) {
        fprintf(stderr, "kernel should be less than 190 MB.\n");
        ret = -1;
        goto end;
    }
    if (bz_image_size <= 128 * 1024) {
        fprintf(stderr, "kernel should be at least 128 KB.\n");
        ret = -1;
        goto end;
    }
    // Setup boot parameters at 0x10000
    zeropage = (struct boot_params *)((uint8_t *)m_mem_map->host_mem + 0x10000);
    memset(zeropage, 0, sizeof(*zeropage));
    memcpy(&zeropage->hdr, (uint8_t *)bz_image + 0x01f1, sizeof(zeropage->hdr));
    // Setup E820 memory map
    setup_e820_map(zeropage);
    // Setup kernel loader info
    zeropage->hdr.type_of_loader = 0xFF;
    zeropage->hdr.loadflags |= LOADED_HIGH;
    zeropage->hdr.vid_mode = 0xFFFF;
    zeropage->hdr.cmd_line_ptr = 0x20000;
    // Copy command line
    cmd_line = (char *)((uint8_t *)m_mem_map->host_mem + 0x20000);
    cmdline_buf = strdup(kernel_args);
    if (m_blk) {
        cmdline_buf = cmdline_concat(cmdline_buf, VIRTIO_BLK_CMDLINE);
        if (cmdline_buf == NULL) {
            fprintf(stderr, "invalid kernel args.\n");
            ret = -1;
            goto end;
        }
    }
    if (m_net) {
        cmdline_buf = cmdline_concat(cmdline_buf, VIRTIO_NET_CMDLINE);
        if (cmdline_buf == NULL) {
            fprintf(stderr, "invalid kernel args.\n");
            ret = -1;
            goto end;
        }
    }
    if (strnlen(cmdline_buf, 2000) >= 2000) {
        fprintf(stderr, "invalid kernel args.\n");
        free(cmdline_buf);
        ret = -1;
        goto end;
    }
    memcpy(cmd_line, cmdline_buf, strnlen(cmdline_buf, 2000) + 1);
    free(cmdline_buf);
    // Load initrd
    if (load_initrd(zeropage, initrd_path) < 0) {
        fprintf(stderr, "failed to load initrd\n");
        ret = -1;
        goto end;
    }
    // Copy protected mode kernel to 1MB
    setup_size = (zeropage->hdr.setup_sects + 1) * 512;
    memcpy((uint8_t *)m_mem_map->host_mem + 0x100000, (char *)bz_image + setup_size,
           bz_image_size - setup_size);
    // cleanup
end:
    if (bz_image) {
        munmap(bz_image, bz_image_size);
    }
    return ret;
}

int mvvm::handle_power(struct kvm_run *run)
{
    uint8_t *io_data = (uint8_t *)run + run->io.data_offset;
    if (run->io.direction == KVM_EXIT_IO_IN) {
        *io_data = m_power_cmd;
        m_power_cmd = 0;
        return 0;
    }
    if (run->io.direction == KVM_EXIT_IO_OUT) {
        if (*io_data == 1) {
            return *io_data;
        }
        return 0;
    }
    return 0;
}

int mvvm::run()
{
    int ret = 0;
    int mmap_size = 0;
    struct kvm_run *run = NULL;
    virtio_device *virtiodev = NULL;
    uint64_t mmio_base_addr = 0;

    mmap_size = ioctl(m_kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (mmap_size < 0) {
        perror("KVM_GET_VCPU_MMAP_SIZE");
        exit(-1);
    }
    // Map the shared memory region
    run =
        (struct kvm_run *)mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, m_cpu_fd, 0);
    if (run == MAP_FAILED) {
        perror("mmap kvm_run");
        exit(-1);
    }
    while (1) {
        if (ioctl(m_cpu_fd, KVM_RUN, 0) < 0) {
            if (errno == EINTR)
                continue;
            perror("KVM_RUN");
            break;
        }
        switch (run->exit_reason) {
        case KVM_EXIT_IO:
            if (run->io.port >= 0x3f8 && run->io.port <= 0x3ff) {
                m_serial->handle_io_event(run);
            }
            if (run->io.port == 0x300) {
                ret = handle_power(run);
                if (ret != 0) {
                    goto exit_loop;
                }
            }
            break;
        case KVM_EXIT_SHUTDOWN:
            printf("KVM_EXIT_SHUTDOWN\n");
            ret = 1;
            goto exit_loop;
        case KVM_EXIT_MMIO: {
            if (run->mmio.phys_addr >> 30 == 1024) {
                virtiodev = m_blk;
                mmio_base_addr = VIRTIO_BLK_MMIO_ADDR;
            } else if (run->mmio.phys_addr >> 30 == 1025) {
                virtiodev = m_net;
                mmio_base_addr = VIRTIO_NET_MMIO_ADDR;
            } else {
                break;
            }
            uint32_t offset = run->mmio.phys_addr - mmio_base_addr;
            if (offset > 4096)
                break;
            if (run->mmio.is_write) {
                DEBUG("mmio write, addr: 0x%x, data: 0x%x\n",
                      (int)(run->mmio.phys_addr - mmio_base_addr), *(uint32_t *)(run->mmio.data));
                virtio_mmio_write(virtiodev, offset, *(uint32_t *)(run->mmio.data), run->mmio.len);
            } else {
                uint32_t val = virtio_mmio_read(virtiodev, offset, run->mmio.len);
                DEBUG("mmio read, addr: 0x%x, data: 0x%x\n",
                      (int)(run->mmio.phys_addr - mmio_base_addr), val);
                *(uint32_t *)(run->mmio.data) = val;
            }
            break;
        }
        default:
            printf("Unhandled exit reason: %d\n", run->exit_reason);
            ret = 1;
            goto exit_loop;
        }
    }
exit_loop:
    munmap(run, mmap_size);
    return ret;
}

int mvvm::set_irq(int irq, int level)
{
    struct kvm_irq_level irq_level = {0};
    irq_level.irq = irq;
    irq_level.level = level;
    if (ioctl(m_vm_fd, KVM_IRQ_LINE, &irq_level) < 0) {
        perror("KVM_IRQ_LINE");
        return -1;
    }
    return 0;
}

void mvvm::shutdown()
{
    m_power_cmd = 1;
    set_irq(5, 1);
    set_irq(5, 0);
}

} // namespace mvvmm
