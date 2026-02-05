# Minimal Viable Virtual Machine Monitor

A small VMM built on KVM.

![Snipaste_2026-02-04_15-36-33](https://github.com/user-attachments/assets/3b52cb10-e57a-437c-b675-f90a0535ed34)

## Build

    make

## Test

Get a vmlinuz and initrd, then:

    ./mvvmm vmlinuz initrd

## TODO

- virtio-blk emulation
- virtio-net emulation


## License

All files are licensed under GPLv3 if not specified otherwise.

`virtio.c` and `virtio.h` (ported from TinyEMU) are licensed under MIT license.
