#ifndef MVVMM_SERIAL_H_
#define MVVMM_SERIAL_H_

#include <pthread.h>
#include <stdint.h>

struct serial {
    uint8_t regs[8];
    uint8_t dl[2];
    pthread_mutex_t rx_lock;
    pthread_cond_t rx_cond;
};
struct kvm_run;
struct mvvm;

void serial_init(struct serial *self);
void handle_serial(struct mvvm *vm, struct kvm_run *run);
void write_to_serial(struct mvvm *vm, char c);
void serial_destroy(struct serial *self);

#endif