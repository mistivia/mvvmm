#ifndef MVVMM_BLKDEV_H_
#define MVVMM_BLKDEV_H_

int
mvvm_init_virtio_blk(struct mvvm *self, const char *disk_path);

#endif