#ifndef MVVMM_SERIAL_H_
#define MVVMM_SERIAL_H_

#include <stdint.h>

struct serial {
    uint8_t regs[8];
    uint8_t dll, dlh;
};
struct kvm_run;
struct mvvm;

void serial_init(struct serial *self);
void handle_serial(struct mvvm *vm, struct kvm_run *run);

#endif