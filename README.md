# MVVMM – Minimal Viable Virtual Machine Monitor

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![Platform: Linux](https://img.shields.io/badge/Platform-Linux-lightgrey)](https://www.kernel.org/)
[![Arch: x86_64](https://img.shields.io/badge/Arch-x86__64-green)](https://en.wikipedia.org/wiki/X86-64)
[![Requires: KVM](https://img.shields.io/badge/Requires-KVM-orange)](https://www.linux-kvm.org/)

A lightweight, hackable Virtual Machine Monitor (VMM) built on KVM. Designed for experimentation and learning, not ready for production use.

**Features**:
- Boot Linux with minimal overhead
- Virtio‑based block and network devices
- Graceful shutdown via guest module
- Performance optimizations: `irqfd`, `ioeventfd`, virtqueue interrupt suppression
- Run AI agents (Codex, Claude Code, OpenClaw, etc.)

## Table of Contents

- [Quick Start](#quick-start)
- [Prerequisites](#prerequisites)
- [Building](#building)
- [Creating a Guest](#creating-a-guest)
- [Running the VM](#running-the-vm)
- [Power Management](#power-management)
- [Screenshot](#screenshot)
- [License](#license)

## Quick Start

For a quick test, follow these steps:

1. Clone the repository and build the VMM:

```bash
git clone --depth 1 https://github.com/mistivia/mvvmm
cd mvvmm
make
```

2. Prepare a Linux kernel with VirtIO‑MMIO support (see [Creating a Guest](#creating-a-guest) for details).

3. Create a disk image (Alpine Linux minirootfs for example) and a simple initramfs.

4. Run the VM:

```bash
./mvvmm -k vmlinuz -i initrd -d disk.img -t tap0
```

The following sections provide detailed instructions for each step.

## Prerequisites

- A Linux host with KVM support (`/dev/kvm` accessible)
- GCC, Make, and standard build tools
- Root privileges for network setup and disk mounting
- Enough RAM and disk space for the guest

## Building

Compile `mvvmm` with a single command:

```bash
make
```

The binary `mvvmm` will be produced in the current directory.

## Creating a Guest

### 1. Linux Kernel

Download and build a recent Linux kernel (6.18.9 used in this example):

```bash
wget https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.18.9.tar.xz
tar -xf linux-6.18.9.tar.xz
cd linux-6.18.9
```

Enable the necessary VirtIO‑MMIO options:

```bash
make defconfig
make menuconfig
```

Search for and enable:

- `CONFIG_VIRTIO_MMIO`
- `CONFIG_VIRTIO_MMIO_CMDLINE_DEVICES`

Then compile the kernel:

```bash
make -j$(nproc)
cp arch/x86/boot/bzImage ../vmlinuz
cd ..
```

### 2. Initial RAM Disk (initrd)

Create a minimal initramfs with BusyBox:

```bash
wget https://busybox.net/downloads/binaries/1.35.0-x86_64-linux-musl/busybox
mkdir -p initramfs/bin
chmod +x busybox
./busybox --install initramfs/bin/
```

Write an `init` script at `initramfs/init`:

```sh
#!/bin/sh

mount -t devtmpfs devtmpfs /dev
mount -t proc proc /proc
mount -t sysfs sys /sys
mkdir -p /dev/pts
mount -t devpts devpts /dev/pts
mdev -s

mkdir /sysroot
mount /dev/vda /sysroot
mount --move /dev /sysroot/dev
mount --move /proc /sysroot/proc
mount --move /sys /sysroot/sys

ip link set eth0 up
ip addr add 192.168.200.2/24 dev eth0
ip route add default via 192.168.200.1

exec chroot /sysroot /sbin/init
```

Make it executable and pack the initrd:

```bash
cd initramfs
chmod +x init
find . -print0 | cpio --null -ov --format=newc | gzip > ../initrd
cd ..
```

### 3. Disk Image

Create a sparse disk image and install Alpine Linux:

```bash
dd if=/dev/zero of=disk.img bs=1 count=0 seek=15G
mkfs.ext4 disk.img
mkdir mnt
sudo mount disk.img ./mnt
```

Extract Alpine minirootfs:

```bash
cd mnt
wget https://dl-cdn.alpinelinux.org/alpine/v3.23/releases/x86_64/alpine-minirootfs-3.23.3-x86_64.tar.gz
tar -xf alpine-minirootfs-3.23.3-x86_64.tar.gz
cd ..
```

Chroot into the image for basic setup:

```bash
sudo chroot ./mnt
export PATH=/bin:/sbin:$PATH
passwd
echo 'nameserver 8.8.8.8' > ./etc/resolv.conf
apk add openrc fastfetch
```

Enable serial console by uncommenting the following line in `/etc/inittab`:

```
ttyS0::respawn:/sbin/getty -L 115200 ttyS0 vt100
```

Exit chroot and unmount:

```bash
exit
sudo umount ./mnt
```

### 4. Network Setup

Create a TAP device and enable NAT:

```bash
ip tuntap add dev tap0 mod tap
ip link set dev tap0 up
ip addr add 192.168.200.1/24 dev tap0

sysctl -w net.ipv4.ip_forward=1
iptables -A FORWARD -i tap0 -j ACCEPT
iptables -A FORWARD -o tap0 -j ACCEPT
iptables -t nat -A POSTROUTING -o [YOUR_INTERNET_INTERFACE] -j MASQUERADE
```

Replace `[YOUR_INTERNET_INTERFACE]` with your host's outward‑facing interface (e.g., `eth0`, `wlan0`).

## Running the VM

Launch the virtual machine with the kernel, initrd, disk image, and TAP device:

```bash
./mvvmm -k vmlinuz -i initrd -d disk.img -t tap0
```

You will see the serial console. Log in with the root password you set during the chroot step.

Once logged in, you can run `fastfetch` to verify the system is operational.

To shut down the guest, issue:

```bash
poweroff
```

Then press `Ctrl+A Ctrl+C` in the terminal to exit the VMM.

## Power Management

`mvvmm` does not have ACPI. A small Linux kernel module (`guest-module/`) handles poweroff and graceful shutdown.

When the host receives SIGTERM, the guest module will shut down the guest gracefully.

Reboot is not supported; restart the virtual machine process if you need to reboot.

## Screenshot

<img width="811" height="714" alt="image" src="https://github.com/user-attachments/assets/92bc0333-1d8b-461b-aba3-5871be9746d7" />

## License

All files are licensed under GPLv3 if not specified otherwise.

`virtio.c` and `virtio.h` (ported from TinyEMU) are licensed under MIT license.
