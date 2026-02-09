#ifndef MVVMM_BLKDEV_H_
#define MVVMM_BLKDEV_H_

int
mvvm_init_virtio_blk(struct mvvm *self, const char *disk_path);

void mvvm_destroy_virtio_blk(struct mvvm *self);

#endif