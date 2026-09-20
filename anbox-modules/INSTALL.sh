#!/usr/bin/env bash

# First install the configuration files:
sudo cp anbox.conf /etc/modules-load.d/
sudo cp 99-anbox.rules /lib/udev/rules.d/

# Then copy the ashmem module sources to /usr/src/:
sudo cp -rT ashmem /usr/src/anbox-ashmem-1

# Finally use dkms to build and install ashmem:
sudo dkms install anbox-ashmem/1

# Modern kernels provide binder through the in-tree driver and binderfs. Only
# fall back to the legacy out-of-tree binder module if binderfs is missing.
if ! grep -qw binder /proc/filesystems; then
    sudo cp -rT binder /usr/src/anbox-binder-1
    sudo dkms install anbox-binder/1
fi

# Verify by loading kernel support and checking the created devices:
sudo modprobe ashmem_linux
if grep -qw binder /proc/filesystems; then
    sudo mkdir -p /dev/binderfs
    if ! grep -qs '[[:space:]]/dev/binderfs[[:space:]]binder[[:space:]]' /proc/mounts; then
        sudo mount -t binder binder /dev/binderfs
    fi
else
    sudo modprobe binder_linux
fi
lsmod | grep -e ashmem_linux -e binder_linux || true
ls -alh /dev/binder /dev/binderfs/binder-control /dev/ashmem 2>/dev/null || true
