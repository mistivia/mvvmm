#include "mvvm.h"

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

static void set_flat_mode(struct kvm_segment *seg) {
    seg->base = 0;
    seg->limit = 0xffffffff;
    seg->g = 1;
    seg->db = 1;
}

static int init_cpu(int kvm_fd, int cpu_fd) {
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
    cpuid = malloc(sizeof(*cpuid) + 
                   max_entries * sizeof(struct kvm_cpuid_entry2));
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

int mvvm_init(struct mvvm *self, uint64_t mem_size) {
    struct kvm_pit_config pit = {0};
    struct kvm_userspace_memory_region mem = {0};
    
    // Open KVM device
    self->kvm_fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (self->kvm_fd < 0) {
        fprintf(stderr, "failed to open /dev/kvm\n");
        return -1;
    }
    // Create virtual machine
    self->vm_fd = ioctl(self->kvm_fd, KVM_CREATE_VM, 0);
    if (self->vm_fd < 0) {
        fprintf(stderr, "failed to create vm\n");
        return -1;
    }
    // Create IRQ chip for interrupt handling
    if (ioctl(self->vm_fd, KVM_CREATE_IRQCHIP, 0) < 0) {
        fprintf(stderr, "failed to create irqchip\n");
        return -1;
    }
    // Create PIT for timer interrupts
    if (ioctl(self->vm_fd, KVM_CREATE_PIT2, &pit) < 0) {
        fprintf(stderr, "failed to create pit\n");
        return -1;
    }
    // Allocate guest memory
    self->memory_size = mem_size;
    self->memory = mmap(NULL, self->memory_size, 
                        PROT_READ | PROT_WRITE, 
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, 
                        -1, 0);
    if (self->memory == MAP_FAILED) {
        fprintf(stderr, "failed to mmap memory\n");
        return -1;
    }
    // Register memory region with KVM
    mem.slot = 0;
    mem.flags = 0;
    mem.guest_phys_addr = 0;
    mem.memory_size = self->memory_size;
    mem.userspace_addr = (uint64_t)self->memory;
    if (ioctl(self->vm_fd, KVM_SET_USER_MEMORY_REGION, &mem) < 0) {
        fprintf(stderr, "failed to set user memory region\n");
        return -1;
    }
    // Create virtual CPU
    self->cpu_fd = ioctl(self->vm_fd, KVM_CREATE_VCPU, 0);
    if (self->cpu_fd < 0) {
        fprintf(stderr, "failed to create vcpu\n");
        return -1;
    }
    if (init_cpu(self->kvm_fd, self->cpu_fd) < 0) {
        fprintf(stderr, "cpu init failed.\n");
        return -1;
    }
    // Initialize serial port
    serial_init(&self->serial);
    return 0;
}

void mvvm_destroy(struct mvvm *self) {
    close(self->cpu_fd);
    munmap(self->memory, self->memory_size);
    close(self->vm_fd);
    close(self->kvm_fd);
}

static int
map_file(const char *path, size_t *size, void**out)
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
static void
setup_e820_map(struct mvvm *vm, struct boot_params *zeropage)
{
    zeropage->e820_entries = 2;
    // first 640KB
    zeropage->e820_table[0].addr = 0;
    zeropage->e820_table[0].size = 0xA0000;
    zeropage->e820_table[0].type = 1;
    // > 1MB
    zeropage->e820_table[1].addr = 0x100000;
    zeropage->e820_table[1].size = vm->memory_size - 0x100000;
    zeropage->e820_table[1].type = 1;
}

// Load initrd into guest memory at 512MB mark.
// initrd_path is guaranteed to exist.
static int
load_initrd(struct mvvm *vm, struct boot_params *zeropage,
            const char *initrd_path) 
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
    if ((uint64_t)initrd_addr + st.st_size >= vm->memory_size) {
        fprintf(stderr, "failed to load initrd.\n");
        return -1;
    }
    memcpy(vm->memory + initrd_addr, initrd, st.st_size);
    zeropage->hdr.ramdisk_image = initrd_addr;
    zeropage->hdr.ramdisk_size = st.st_size;
    // cleanup
    munmap(initrd, st.st_size);
    close(fd);
    return 0;
}

int
mvvm_load_kernel(struct mvvm *vm, const char *kernel_path,
            const char *initrd_path, const char *kernel_args)
{
    void *bz_image = NULL;
    size_t bz_image_size = 0;
    uint32_t setup_size = 0;
    struct boot_params *zeropage = NULL;
    char *cmd_line = NULL;

    if (map_file(kernel_path, &bz_image_size, &bz_image) < 0) {
        fprintf(stderr, "failed to load kernel.\n");
        return -1;
    }
    // Setup boot parameters at 0x10000
    zeropage = (struct boot_params *)(vm->memory + 0x10000);
    memset(zeropage, 0, sizeof(*zeropage));
    memcpy(&zeropage->hdr, bz_image+0x01f1, sizeof(zeropage->hdr));
    // Setup E820 memory map
    setup_e820_map(vm, zeropage);
    // Setup kernel loader info
    zeropage->hdr.type_of_loader = 0xFF;
    zeropage->hdr.loadflags |= LOADED_HIGH;
    zeropage->hdr.vid_mode = 0xFFFF;
    zeropage->hdr.cmd_line_ptr = 0x20000;
    // Copy command line
    cmd_line = (char *)(vm->memory + 0x20000);
    memcpy(cmd_line, kernel_args, strlen(kernel_args) + 1);
    // Load initrd
    if (load_initrd(vm, zeropage, initrd_path) < 0) {
        fprintf(stderr, "failed to load initrd\n");
        return -1;
    }
    // Copy protected mode kernel to 1MB
    setup_size = (zeropage->hdr.setup_sects + 1) * 512;
    memcpy(vm->memory + 0x100000,
           (char *)bz_image + setup_size,
           bz_image_size - setup_size);
    // cleanup
    munmap(bz_image, bz_image_size);
    return 0;
}

int mvvm_run(struct mvvm *vm) {
    int ret = 0;
    int mmap_size = 0;
    struct kvm_run *run = NULL;
    mmap_size = ioctl(vm->kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (mmap_size < 0) {
        perror("KVM_GET_VCPU_MMAP_SIZE");
        exit(-1);
    }
    // Map the shared memory region
    run = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, 
               vm->cpu_fd, 0);
    if (run == MAP_FAILED) {
        perror("mmap kvm_run");
        exit(-1);
    }
    while (1) {
        if (ioctl(vm->cpu_fd, KVM_RUN, 0) < 0) {
            perror("KVM_RUN");
            break;
        }
        switch (run->exit_reason) {
        case KVM_EXIT_IO:
            if (run->io.port >= 0x3f8 && run->io.port <= 0x3ff) {
                handle_serial(vm, run);
            }
            break;
        case KVM_EXIT_SHUTDOWN:
            printf("KVM_EXIT_SHUTDOWN\n");
            goto exit_loop;
        case KVM_EXIT_MMIO:
            continue;
        default:
            printf("Unhandled exit reason: %d\n", run->exit_reason);
            ret = -1;
            goto exit_loop;
        }
    }
exit_loop:
    munmap(run, mmap_size);
    return ret;
}