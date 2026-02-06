# Minimal Viable Virtual Machine Monitor

A small VMM on KVM with just enough features.

## Build

    make

## Linux Kernel Requirements

Kernel must be built with following configurations:

- CONFIG_SERIAL_8250
- CONFIG_VIRTIO_MMIO
- CONFIG_VIRTIO_MMIO_CMDLINE_DEVICES
- CONFIG_VIRTIO_BLK
- CONFIG_VIRTIO_NET

## Test

Get a vmlinuz and initrd, then:

    ./mvvmm -k vmlinuz -i initrd -m 1G

## Current Status

- [x] Boot Linux
- [x] Support virtio-blk
- [ ] Support virtio-net

## Screenshot

<img height="300px" alt="image" src="https://github.com/user-attachments/assets/45422e2c-f639-4508-89f8-4c7a92c28d95" />

## License

All files are licensed under GPLv3 if not specified otherwise.

`virtio.c` and `virtio.h` (ported from TinyEMU) are licensed under MIT license.
