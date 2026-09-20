# Memory containment and optional swap

Anbox Reboxed can apply cgroup v2 limits to the Android container and can manage one
explicitly configured host swapfile. All controls are disabled by default.
Create `/etc/anbox/memory.conf` (or `/var/lib/anbox/memory.conf`) when limits or
swap are wanted:

```ini
android.memory.high=6G
android.memory.max=10G
android.memory.swap.max=16G
android.swap.enabled=false
android.swap.file=/var/lib/anbox/anbox.swap
android.swap.file_size=16G
```

`memory.high`, `memory.max`, and `memory.swap.max` are applied to the LXC
payload cgroup when the container starts. Values accept `K`, `M`, `G`, `T`,
their `iB` variants, raw bytes, or `max`. `memory.high` must not exceed
`memory.max`.

The swapfile lifecycle is explicit:

```sh
sudo anbox-reboxed swap-create
sudo anbox-reboxed swap-enable
anbox-reboxed memory-status
sudo anbox-reboxed swap-disable
```

`swap-create` uses full preallocation, mode `0600`, a temporary file, and an
atomic rename. It sets NOCOW before allocating on Btrfs. It does not recreate
an unchanged initialized file, resize an existing file, or overwrite a file
that is not already swap. `swap-enable` lets the kernel perform the final
filesystem/extent validation. A configured `android.swap.enabled=true` enables
an existing initialized file when the container starts; it never creates one.

Swap is host-global. The file is not exclusively owned by Android, and Anbox Reboxed
does not change or disable any other host swap device. The cgroup v2
`memory.swap.max` value provides Android containment. Heavy swap use reduces
responsiveness and increases storage writes, particularly on SSDs. Choose
limits and file size for the host; there is intentionally no large default.

`anbox-reboxed memory-status` reports host RAM/swap use, configured values, whether the
configured file is active, and the live Anbox Reboxed cgroup's `memory.current`,
`memory.peak`, `memory.swap.current`, and limit files.
