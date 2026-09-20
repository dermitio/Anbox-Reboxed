# Install and upgrade Anbox Reboxed

Run the transactional installer from the checkout directory that contains it:

```sh
./anbox-reboxed-install.sh --install
./anbox-reboxed-install.sh --update
./anbox-reboxed-install.sh --update-bridge
./anbox-reboxed-install.sh --verify
./anbox-reboxed-install.sh --rollback
./anbox-reboxed-install.sh --uninstall
```

Running `./anbox-reboxed-install.sh` without arguments opens the same actions
as a numbered interactive menu. Option 7 composes a new Android image, option
8 is the optional full AOSP fallback build, and option 9 adds or updates the
Bridge in the installed Android image. Set `ANBOX_REBOXED_ANDROID_BUILD_TOP`
when the fallback AOSP checkout is elsewhere.

The installer resolves the checkout from its own location, builds in the
checkout's `build` directory, stages CMake output, backs up every
replaced host file, records hashes, updates the existing systemd and D-Bus
registrations in place, and verifies installed files, renderer dependencies,
native-bridge state, and PackageManager when an Android session is running.
The installer composes a copy of the supplied Android system image with a
separately supplied platform-signed Reboxed Bridge APK. The resulting image
must contain `/system/priv-app/ReboxedBridge/ReboxedBridge.apk` and its policy
XML, with a certificate matching the image platform certificate.
The public release does not require or include Berberis. See
[public-build.md](public-build.md) for the complete external prerequisites.

Portable source bundles may override discovered paths without editing the
installer:

```sh
ANBOX_REBOXED_SOURCE_DIR=/path/to/anbox \
ANBOX_REBOXED_BUILD_DIR=/path/to/build \
ANBOX_REBOXED_ANDROID_IMAGE=/path/to/system.img \
ANBOX_REBOXED_VENDOR_IMAGE=/path/to/vendor.img \
./anbox-reboxed-install.sh --install
```

`ANBOX_REBOXED_ROOT`, `ANBOX_REBOXED_ICON`,
`ANBOX_REBOXED_MODULE_RULES`, and `ANBOX_REBOXED_BUILD_JOBS` are also
supported. Legacy kernel-module udev rules are optional on systems using
modern binderfs.

### Add or update only the Reboxed Bridge

Use the dedicated action when the installed Android image and Native Bridge
payload should remain otherwise unchanged:

```sh
./anbox-reboxed-install.sh --update-bridge
./anbox-reboxed-install.sh --update-bridge --bridge-apk /path/to/ReboxedBridge.apk
```

The default APK is `build/android/ReboxedBridge.apk`; it can also be selected
with `ANBOX_REBOXED_BRIDGE_APK`. The installer requires a regular, readable
APK, verifies that its certificate matches the platform certificate embedded
in the installed image, and installs it together with the two required
permission XML files. If all three files already match, the action is a no-op.

The update is staged and validated before `/var/lib/anbox/android.img` is
replaced. The installer records the previous image and any legacy runtime
Bridge overlay in its normal rollback backup. It restarts the container only
if the container-manager service was active, then checks PackageManager if an
Android container had been running. `/var/lib/anbox/data`, `vendor.img`, and
the image-resident Native Bridge payload are not changed. Use `--dry-run` to
validate inputs and report the intended operation without modifying the host.

### gfxstream source discovery

The installer treats the active workspace and its CMake configuration as
authoritative. It discovers gfxstream headers and the backend library from
explicit `ANBOX_REBOXED_GFXSTREAM_INCLUDE_DIR` and
`ANBOX_REBOXED_GFXSTREAM_LIBRARY` settings, the active `CMakeCache.txt`,
pkg-config, and current repository source trees. It validates the current
`render-utils` interface headers instead of assuming a historical checkout
layout.

It never fetches, reclones, resets, cleans, or replaces an existing source
tree. The old ColorBuffer/GLES translator patch is considered only when both
of its original targets exist. If current source already provides the needed
capability or uses a different implementation, the installer reports that the
legacy patch is obsolete and continues without modifying source files.

Set `ANBOX_REBOXED_GFXSTREAM_EXPECTED_REVISION` only when a deployment needs
an explicit revision policy. The installer distinguishes an exact revision,
descendant/local commits, a dirty tree, an unrelated revision, and a non-Git
tree. Unverified trees require confirmation, or
`ANBOX_ALLOW_SOURCE_MISMATCH=1` for scripts and other non-interactive runs.

Useful safe modes are:

```sh
./anbox-reboxed-install.sh --update --dry-run
./anbox-reboxed-install.sh --update --skip-build --skip-image-modification
./anbox-reboxed-install.sh --verify-only
```

## Rename migration

An install or update detects an existing `/usr/local/bin/anbox` installation
and migrates it without moving or deleting runtime data. It installs:

- `/usr/local/bin/anbox-reboxed`, with `anbox` as a compatibility symlink
- `/usr/local/bin/anbox-reboxed-window`, with `anbox-window` as a symlink
- rebranded desktop entries and the `anbox-reboxed` icon
- the existing `anbox-container-manager.service` and `org.anbox` D-Bus name,
  updated to launch the canonical executable

The installer writes a rename notice under
`/var/lib/anbox/revival-install/RENAMED_TO_ANBOX_REBOXED` and the system log.
Keeping the old service, D-Bus, desktop filenames, and package/snap identity
allows upgrades to replace registrations in place instead of leaving parallel
installations.

The following paths deliberately remain persistent and are never treated as
disposable old-package data:

- `/var/lib/anbox/android.img`, `vendor.img`, `data`, state, and backups
- `/etc/anbox` configuration
- `${XDG_CONFIG_HOME:-~/.config}/anbox/window.conf`
- `${XDG_STATE_HOME:-~/.local/state}/anbox/session.log`
- `org.anbox`, `anbox0`, qemu-pipe names, LXC names, and Android interfaces

Uninstall removes only files recorded in the current install manifest. Shared
images, user data, configuration state, migration notices, and backups remain.
During an update, a legacy `android.img` that lacks the mandatory Bridge
preload is backed up and replaced only by a source image that passes preload
and platform-signature verification.

## Desktop launcher

The application menu exposes `Anbox Reboxed`, portrait, and landscape entries.
The reference X11 profile is 720×1600 at 320 DPI; host window dimensions and
Android dimensions remain independent.

```sh
anbox-reboxed display-status
anbox-reboxed-window --orientation portrait --width 720 --height 1600 --save
anbox-reboxed-window --orientation landscape
```

## Container networking

The container manager attaches Android to the host's `anbox0` bridge with an
LXC veth. Android receives a static Ethernet configuration while the host
bridge supplies forwarding and NAT. The qemu-pipe, audio, management, and
Reboxed Bridge services remain pathname Unix sockets bind-mounted under
`/dev/anbox_sockets`; they do not require Android to share the host network
namespace.

If Android reports no default network, confirm that `anbox0` exists and that
the running container has an Ethernet interface, address, default route, and
DNS configuration. A container with only `lo` was started without its veth and
cannot use the host's bridge or NAT rules.

## Recovery

`--verify` validates file hashes, compatibility aliases, renderer dependency
resolution, native-bridge state, the container service, PackageManager when a
session is running, and input isolation. `--rollback` restores the most recent host-file
backup without touching Android data. Backups are stored below
`/var/lib/anbox/revival-install/backups/`.
