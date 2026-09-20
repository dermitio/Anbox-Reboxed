#!/bin/sh

set -eu

run_root() {
	if [ "$(id -u)" -eq 0 ]; then
		"$@"
	elif command -v sudo >/dev/null 2>&1; then
		sudo "$@"
	else
		echo "Need root privileges to run: $*" >&2
		return 1
	fi
}

mount_binderfs() {
	if ! grep -qw binder /proc/filesystems; then
		return 1
	fi

	run_root mkdir -p /dev/binderfs
	if ! grep -qs '[[:space:]]/dev/binderfs[[:space:]]binder[[:space:]]' /proc/mounts; then
		run_root mount -t binder binder /dev/binderfs
	fi
}

if ! mount_binderfs && [ ! -e /dev/binder ]; then
	run_root modprobe binder_linux
	[ -e /dev/binder ] && run_root chmod 0666 /dev/binder
fi

run_root modprobe ashmem_linux
[ -e /dev/ashmem ] && run_root chmod 0666 /dev/ashmem
