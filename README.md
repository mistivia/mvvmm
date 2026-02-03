# Minimal Viable Virtual Machine Monitor

A small VMM built on KVM.

## Build

    make

## Test

Get a vmlinuz and initrd, then:

    ./mvvmm vmlinuz initrd

## TODO

- Full 8250 UART emulation
- virtio-blk emulation
- virtio-net emulation