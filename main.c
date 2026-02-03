#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include "mvvm.h"
#include "serial.h"

#define MEM_SIZE (1024*1024*1024)
#define KERNEL_ARGS "console=ttyS0 debug"

int main(int argc, char *argv[]) {
    struct mvvm vm;
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <kernel> <initrd>\n", argv[0]);
        return -1;
    }
    if (mvvm_init(&vm, MEM_SIZE) < 0) {
        return -1;
    }
    if (mvvm_load_kernel(&vm, argv[1], argv[2], KERNEL_ARGS) < 0) {
        return -1;
    }
    int ret;
    if ((ret = mvvm_run(&vm)) != 0) {
        fprintf(stderr, "mvvm exit code: %d\n", ret);
        return ret;
    }
    mvvm_destroy(&vm);
    return 0;
}