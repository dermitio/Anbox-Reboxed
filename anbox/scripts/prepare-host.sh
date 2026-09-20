#!/usr/bin/env bash
set -euo pipefail

if [[ "${1:-}" == "--gsi" ]]; then
  :
elif [[ $# -gt 0 ]]; then
  echo "Usage: $0 [--gsi]" >&2
  exit 2
fi

modprobe loop
# Linux removed the staging ashmem driver after Android switched to memfd.
# Keep using /dev/ashmem when an out-of-tree module is available for legacy
# guests, but do not prevent modern GSIs from booting without it.
if modinfo ashmem_linux >/dev/null 2>&1; then
  modprobe ashmem_linux
fi
modprobe fuse
modprobe tun
modprobe iptable_filter
modprobe iptable_mangle
modprobe iptable_raw
modprobe iptable_nat
modprobe ip6table_filter
modprobe ip6table_mangle
modprobe ip6table_raw
modprobe ip6table_nat
modprobe xt_bpf
modprobe xt_NFLOG
modprobe xt_u32
modprobe xt_state
modprobe xt_policy
modprobe xt_LOG
modprobe nf_conntrack_netlink
modprobe xt_TCPMSS
modprobe ipt_REJECT
modprobe ip6t_REJECT
modprobe xt_tcpudp
modprobe xt_multiport

# Android 15's BpfNetMaps uses a short-lived PF_KEY socket to synchronize
# kernel RCU before replacing network-policy maps. Without af_key, netd's
# initialization succeeds but system_server loops on EAFNOSUPPORT.
modprobe af_key

if grep -qw binder /proc/filesystems; then
  # Never allow a legacy out-of-tree binder_linux module to coexist with an
  # in-tree Binder driver. Loading the duplicate module can corrupt Binder's
  # global/debugfs registration and has caused a host kernel crash on Linux 7.
  legacy_binder_module="$(modinfo -n binder_linux 2>/dev/null || true)"
  if [[ -n "${legacy_binder_module}" && "${legacy_binder_module}" != "(builtin)" ]]; then
    echo "Unsafe legacy Binder module installed: ${legacy_binder_module}" >&2
    echo "This kernel already provides Binder. Remove the binder DKMS package before starting Anbox Reboxed." >&2
    exit 1
  fi
  mkdir -p /dev/binderfs
  if ! mountpoint -q /dev/binderfs; then
    mount -t binder binder /dev/binderfs
  fi
elif [[ ! -e /dev/binder ]]; then
  modprobe binder_linux devices=binder
fi

if command -v udevadm >/dev/null 2>&1; then
  udevadm trigger --subsystem-match=misc --action=add
  udevadm settle
fi

for device in /dev/loop-control /dev/fuse /dev/net/tun; do
  if [[ ! -e "${device}" ]]; then
    echo "Required device was not created: ${device}" >&2
    exit 1
  fi
done

if [[ ! -e /dev/binder && ! -e /dev/binderfs/binder-control ]]; then
  echo "Neither a binder device nor BinderFS is available" >&2
  exit 1
fi
