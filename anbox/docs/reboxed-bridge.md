# Reboxed Bridge

Reboxed Bridge connects the Anbox Reboxed host session to a small Android
control panel. It provides clipboard synchronization, bounded file transfer,
safe received-file URIs, split-package installation, intent forwarding, and
Native Bridge diagnostics. The app itself never edits `system.img`,
`vendor.img`, boot properties, `/system`, or `/vendor`.

## Components and trust boundaries

`anbox-reboxed bridge daemon` runs as the desktop user. It creates the
versioned `reboxed_bridge_v1` Unix socket below the existing Anbox runtime
socket directory, which the container already exposes at
`/dev/anbox_sockets`. Do not start this daemon with `sudo`; it refuses to run
as root. The supplied user systemd unit adds process hardening and limits the
service to Unix sockets.

Every connection is checked with Linux `SO_PEERCRED` before a protocol message
is accepted. A peer must then declare its host or guest role using protocol
version 1. The guest role is restricted to the Android system UID. Override
the accepted translated UID explicitly with `--guest-uid` when a different
LXC idmap is configured; do not add ordinary Android application UIDs.

The `ReboxedBridge` Android app is platform signed and installed under
`/system/priv-app` by the Android product build. Its non-exported background
service runs as the Android system UID and is the only Android peer.
Privileged package-install permission is used only inside that service for the
PackageInstaller session; clipboard, document selection, content delivery,
and UI use normal Android APIs. No exported component exposes privileged
operations. The app never writes Android image or property partitions.

The existing Anbox `/dev/anbox_bridge` protobuf/Binder path remains in place
for window management, app launching, and its SDL clipboard implementation.
SDL supplies native X11/Wayland clipboard behavior for the session manager.
The bridge daemon uses `wl-copy`/`wl-paste` or `xclip` for sessions where the
control service runs separately. SHA-256 origin markers suppress reflected
clipboard updates in either direction.

## Running the bridge

```bash
systemctl --user enable --now anbox-reboxed-bridge.service
anbox-reboxed bridge status
anbox-reboxed bridge send ~/Downloads/document.pdf
anbox-reboxed bridge install base.apk split_config.x86_64.apk
```

Received Android exports are placed in
`$XDG_STATE_HOME/anbox-reboxed/bridge/exports` (normally
`~/.local/state/anbox-reboxed/bridge/exports`). Partial inbound data uses a
different `incoming` directory and is atomically renamed only after declared
size and SHA-256 validation. Transfer history is bounded to 200 entries.

Files sent from the host with `bridge send`, including files dropped onto an
Anbox Reboxed window, are published in Android's public `Download/` directory
through MediaStore after size and SHA-256 validation. Existing names are never
overwritten. Files received by older Bridge versions remain in the app's
private storage and are not migrated automatically.

The host never accepts a guest-provided destination path. Names are single
path components; traversal, NULs, symlink components, non-regular input,
oversized files, oversized protocol frames, unsafe APK ZIP members, and ZIP
bombs are rejected. Cancellation closes and deletes the partial file. Newly
received Android files use MediaStore content URIs; the non-exported
`org.anbox.reboxed.bridge.files` provider remains available only for files
received by older versions. Temporary read grants are used when opening either
kind of content, never `file://` URIs.

Before any package bundle is submitted, both the CLI and control panel show:

- package and version;
- selected base and split APKs;
- APK native ABIs;
- natively executable guest ABIs;
- verified translated ABIs;
- whether Native Bridge is required;
- a specific incompatibility reason.

The Android receiver replaces the `Installing` state for success, user-action,
and every PackageInstaller failure status. Split identities and checksums are
validated before one atomic PackageInstaller session is committed.

## External Native Bridge overlays

No translator is included. You must provide translator files that you are
legally authorized to possess and use.

```bash
sudo anbox-reboxed native-bridge install --source /authorized/payload
sudo anbox-reboxed native-bridge enable
# Restart the Android container so immutable ro.* properties take effect.
sudo anbox-reboxed native-bridge verify
sudo anbox-reboxed native-bridge status

sudo anbox-reboxed native-bridge disable
sudo anbox-reboxed native-bridge remove
```

`verify` installs a temporary dual-ABI probe APK with an ABI-specific
PackageInstaller session, launches its instrumentation through Android/ART,
loads the selected ARM JNI library with `System.loadLibrary`, and compares the
returned architecture-specific value. The probe package is uninstalled after
each architecture. It never treats direct execution of an ARM ELF from a guest
shell as Native Bridge verification. ARM ABIs remain absent from the generated
ABI properties until their corresponding Android-process JNI call succeeds.

`install` accepts one translator library, a directory, a zip/tar archive, or
an ext2/ext3/ext4 image. Extraction is local. Archive links, absolute paths,
traversal, special files, unexpected ELF architectures, missing DT_NEEDED
dependencies, writable installed payloads, excessive entry counts, and size
limits fail the transaction. Every output has a SHA-256 entry in the local
manifest.

The payload is staged below `/var/lib/anbox/state/native-bridge`. `enable`
adds read-only bind entries to Anbox's existing `bindtab` and generates a
merged property overlay from the current mounted Android property file. The
original property values are recorded. `disable` removes only Native Bridge
bind entries, so the unmodified property file becomes visible again on the
next boot. `remove` performs `disable` first and deletes the staged overlay.

Detection and verification are intentionally different. Finding a 32-bit or
64-bit translator library records a detected ABI but advertises no ARM ABI.
`verify` checks that the test is a real ARM ELF, copies it into Android data,
executes it inside the running guest, and requires its expected output and a
zero exit status. Only passing architectures are added to the generated ABI
properties, effective after the next container restart. A present library or
property alone is never reported as working.

The ARM test sources are GPL integration tests in
`data/native-bridge-tests`; they are cross-compiled during the host build.
They contain no Google runtime code, translator code, or extracted image
content.

## Building the Android app

`android/reboxed-bridge/Android.bp` defines `ReboxedBridge` as a privileged
platform-certificate app. `products/anbox.mk` includes the app and its
privileged-permission and shared-UID allowlists in the system product image:

```bash
m ReboxedBridge systemimage
```

The resulting image must contain
`/system/priv-app/ReboxedBridge/ReboxedBridge.apk` before it is packed for
Anbox Reboxed. `scripts/create-package.sh` and the host installer both run
`scripts/verify-reboxed-bridge-preload`; packaging fails if the preload path,
root ownership, modes, permission XML, or signing certificate is wrong. The
certificate is compared directly with `framework-res.apk` from the same image.
Do not sideload the Bridge or grant its privileges after boot.

## Licensing

This repository contains only GPL integration code, local extraction and
validation logic, compatibility metadata generated from user input,
checksums, tests, and documentation. It does not contain or redistribute
`libndk_translation`, Houdini, Google ARM runtime files, or files extracted
from proprietary images. Users are responsible for obtaining authorization
for every translator payload they provide.
