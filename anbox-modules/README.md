[![Build Status](https://travis-ci.org/anbox/anbox-modules.svg?branch=master)](https://travis-ci.org/anbox/anbox-modules)

# Anbox Kernel Modules

This repository contains the legacy kernel module support necessary to run the
Anbox Android container runtime on systems that still need it. On modern
kernels, including Linux 7.x, binder is provided by the in-tree binder driver
and binderfs; this tree only needs to build ashmem.

# Install Instruction

You need to have `dkms` and linux-headers on your system. You can install them by
`sudo apt install dkms` or `sudo yum install dkms` (`dkms` is available in epel repo
for CentOS).

Package name for linux-headers varies on different distributions, e.g.
`linux-headers-generic` (Ubuntu), `linux-headers-amd64` (Debian),
`kernel-devel` (CentOS, Fedora), `kernel-default-devel` (openSUSE).


You can either run `./INSTALL.sh` script to automate the installation steps or follow them manually below:

* First install the configuration files:

  ```
  $ sudo cp anbox.conf /etc/modules-load.d/
  $ sudo cp 99-anbox.rules /lib/udev/rules.d/
  ```

* Then copy the ashmem module sources to `/usr/src/`:

  ```
  $ sudo cp -rT ashmem /usr/src/anbox-ashmem-1
  ```

* Finally use `dkms` to build and install ashmem:

  ```
  $ sudo dkms install anbox-ashmem/1
  ```

Binder should come from binderfs on modern kernels. You can verify support with:

```
$ grep -w binder /proc/filesystems
nodev	binder
```

Mount binderfs if your distribution does not do it for you:

```
$ sudo mkdir -p /dev/binderfs
$ sudo mount -t binder binder /dev/binderfs
```

Only use the legacy `binder` DKMS source on kernels without binderfs support.

You can verify by loading ashmem and checking the created devices:

```
$ sudo modprobe ashmem_linux
$ lsmod | grep ashmem_linux
$ ls -alh /dev/ashmem /dev/binderfs/binder-control
```

You are expected to see output like:

```
ashmem_linux           16384  0
crw-rw-rw- 1 root root  10, 55 Jun 19 16:30 /dev/ashmem
crw------- 1 root root 511,  0 Jun 19 16:30 /dev/binderfs/binder-control
```
