# Minimal Viable Virtual Machine Monitor

A small VMM on KVM with just enough features, not ready for production, but easy for hacking and tweaking.

## Current Status

- [x] Boot Linux
- [x] Block device
- [x] Network interface
- [x] Graceful shutdown
- [x] Run Codex/Claude Code/OpenClaw/...
- [ ] Better performance with `ioeventfd` and `irqfd`
- [ ] Better performance with `VIRTIO_F_EVENT_IDX`
- [ ] Symmetric Multiprocessing (SMP) 

## Guide

Download `mvvmm`:

    git clone --depth 1 https://github.com/mistivia/mvvmm

Build:

    cd mvvmm
    make

Download linux kernel source:

    wget https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.18.9.tar.xz
    tar -xf linux-6.18.9.tar.xz
    cd linux-6.18.9

Configure Linux kernel:

    make defconfig
    make menuconfig

Press `/` and search for the following configs, set them to `y`:

    CONFIG_VIRTIO_MMIO
    CONFIG_VIRTIO_MMIO_CMDLINE_DEVICES

Build Linux kernel:

    make -j4
    cp arch/x86/boot/bzImage ../vmlinuz
    cd ..

Setup a TAP device and enable NAT:

    ip tuntap add dev tap0 mod tap
    ip link set dev tap0 up
    ip addr add 192.168.200.1/24 dev tap0

    sysctl -w net.ipv4.ip_forward=1
    iptables -A FORWARD -i tap0 -j ACCEPT
    iptables -A FORWARD -o tap0 -j ACCEPT
    iptables -t nat -A POSTROUTING -o [YOUR_INTERNET_INTERFACE] -j MASQUERADE

Download BusyBox and install it:

    wget https://busybox.net/downloads/binaries/1.35.0-x86_64-linux-musl/busybox
    mkdir -p initramfs/bin
    chmod +x busybox
    ./busybox --install initramfs/bin/

Create a `init` script:

    vim initramfs/init

`init` content:

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

Create initrd:

    cd initramfs
    chmod +x init
    find . -print0 | cpio --null -ov --format=newc | gzip > ../initrd
    cd ..

Download Alpine Linux minirootfs:

    wget https://dl-cdn.alpinelinux.org/alpine/v3.23/releases/x86_64/alpine-minirootfs-3.23.3-x86_64.tar.gz

Create a disk image with hole, and mount it:

    dd if=/dev/zero of=disk.img bs=1 count=0 seek=15G
    mkfs.ext4 disk.img
    mkdir mnt
    sudo mount disk.img ./mnt

Install Alpine Linux, chroot into minirootfs and do some setup:

    cd mnt
    tar -xf ../alpine-minirootfs-3.23.3-x86_64.tar.gz
    cd ..
    sudo chroot ./mnt
    export PATH=/bin:/sbin:$PATH
    passwd
    echo 'nameserver 8.8.8.8' > ./etc/resolv.conf
    apk add openrc fastfetch

Edit `/etc/inittab` and uncomment this line to enable getty on seiral port:

    ttyS0::respawn:/sbin/getty -L 115200 ttyS0 vt100

Exit chroot and unmount disk:

    exit
    sudo umount ./mnt

Start virtual machine:

    ./mvvm -k vmlinuz -i initrd -d disk.img -t tap0

Login and run `fastfetch`:

    fastfetch

When everything is done, shutdown the virtual machine:

    poweroff

And press Ctrl+A Ctrl+C to quit.

You can also set up a `sshd` and disable serial port. With keyboard, mouse, and serial ports disabled, Alpine Linux can boot in 300 ms.

## Power Management

`mvvmm` don't have ACPI. I wrote a small Linux kernel module to handle poweroff. See `guest-module`.

`guest-module` will also provide graceful shutdown with host received SIGTERM.

There is no reboot. Just restart the virtual machine process if you want a reboot.

## Screenshot

<img width="811" height="714" alt="image" src="https://github.com/user-attachments/assets/92bc0333-1d8b-461b-aba3-5871be9746d7" />

## License

All files are licensed under GPLv3 if not specified otherwise.

`virtio.c` and `virtio.h` (ported from TinyEMU) are licensed under MIT license.
