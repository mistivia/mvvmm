# Minimal Viable Virtual Machine Monitor

A small VMM built on KVM.

<img width="789" height="624" alt="image" src="https://github.com/user-attachments/assets/45422e2c-f639-4508-89f8-4c7a92c28d95" />

## Build

    make

## Test

Get a vmlinuz and initrd, then:

    ./mvvmm vmlinuz initrd

## Current Status

- [x] Boot Linux
- [x] Support virtio-blk
- [ ] Support virtio-net


## License

All files are licensed under GPLv3 if not specified otherwise.

`virtio.c` and `virtio.h` (ported from TinyEMU) are licensed under MIT license.
