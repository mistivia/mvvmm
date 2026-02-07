#ifndef MVVMM_NETDEV_H_
#define MVVMM_NETDEV_H_

struct mvvm;

int
mvvm_init_virtio_net(struct mvvm *self, const char *tap_name);

#endif