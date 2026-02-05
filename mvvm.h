#ifndef MVVM_H_
#define MVVM_H_

#include <stdlib.h>

#include "virtio.h"
#include "serial.h"

struct mvvm {
    int kvm_fd;
    int vm_fd;
    int cpu_fd;
    void* memory;
    size_t memory_size;
    struct serial serial;
    VIRTIODevice *blk;
};

int mvvm_init(struct mvvm *vm, uint64_t mem_size);
int mvvm_load_kernel(struct mvvm *vm, const char *kernel_path,
                     const char *initrd_path, const char *kernel_args);
int mvvm_run(struct mvvm *vm);
void mvvm_destroy(struct mvvm *self);

#endif