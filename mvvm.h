#ifndef MVVM_H_
#define MVVM_H_

#include <stdlib.h>

#include "virtio.h"
#include "serial.h"

struct guest_mem_map {
    void *host_mem;
    uint64_t size;
};

struct mvvm {
    int kvm_fd;
    int vm_fd;
    int cpu_fd;
    struct guest_mem_map *mem_map;
    struct serial serial;
    VIRTIODevice *blk;
    VIRTIODevice *net;
    int quit;
    uint8_t power_cmd;
};

int mvvm_init(struct mvvm *vm, uint64_t mem_size, const char *disk, const char *network);
int init_cpu(int kvm_fd, int cpu_fd);
int mvvm_load_kernel(struct mvvm *vm, const char *kernel_path,
                     const char *initrd_path, const char *kernel_args);
int mvvm_run(struct mvvm *vm);
void mvvm_destroy(struct mvvm *self);

// must use with mvvmm guest module
void mvvm_shutdown(struct mvvm *vm);

#endif