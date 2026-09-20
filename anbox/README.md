# Anbox Reboxed

Anbox Reboxed is an unofficial modernization of the archived open-source
Anbox desktop Android runtime. It boots Android 15 in an LXC container and
provides a revised host renderer, bounded keyboard/mouse input pipeline,
networking, compatibility overlays, memory controls, and runtime diagnostics.

This project is not affiliated with Canonical, the original Anbox authors, or
Anbox Cloud. Original source and compatibility components retain their
existing licenses and identifiers.

## Build

The host runtime uses CMake and C++17. Typical dependencies include LXC,
SDL2, EGL/GLES2, Boost, Protobuf, systemd, libcap, GLM, and D-Bus development
packages.

```sh
cmake -S . -B ../build
cmake --build ../build -j4
ctest --test-dir ../build --output-on-failure
```

The bundled installer and `build.sh` use this single outer build directory.
Set `ANBOX_REBOXED_BUILD_DIR` when another out-of-source location is desired.

The canonical executable is `anbox-reboxed`. An `anbox` compatibility wrapper
is installed and generated in the build tree, so existing scripts keep
working.

## Run

The reference X11 Android profile is 720×1600 at 320 DPI:

```sh
anbox-reboxed session-manager --single-window \
  --android-size=720x1600 --density=320 \
  --renderer=reboxed --input-backend=reboxed
```

Useful host commands include:

```sh
anbox-reboxed help
anbox-reboxed version
anbox-reboxed system-info
anbox-reboxed display-status
anbox-reboxed shell
anbox-reboxed logcat -d
anbox-reboxed install application.apk
anbox-reboxed list-package
```

The legacy renderer and input paths remain selectable with
`--renderer=legacy` and `--input-backend=legacy` for comparison.

## Install and upgrade

From the checkout directory containing the transactional installer, run:

```sh
./anbox-reboxed-install.sh --install
./anbox-reboxed-install.sh --update
./anbox-reboxed-install.sh --verify
./anbox-reboxed-install.sh --rollback
./anbox-reboxed-install.sh --uninstall
```

Upgrades detect an existing Anbox installation, replace user-facing surfaces
in place, and retain `/var/lib/anbox`, `/etc/anbox`, `org.anbox`, service names,
and other compatibility-sensitive paths. `anbox` and `anbox-window` remain
aliases for the new commands. Uninstall leaves Android images, user data,
configuration state, and backups intact.

See [installation](docs/install.md), [renderer](docs/renderer.md),
[input](docs/input.md), [memory](docs/memory.md), and
[runtime setup](docs/runtime-setup.md) for details. The
[rename compatibility inventory](docs/rebranding.md) classifies identifiers
that deliberately retain the original spelling.

## Compatibility identifiers

The lowercase `anbox` namespace, D-Bus interfaces, qemu-pipe services, socket
names, Android interfaces, LXC names, environment variables, resource paths,
and configuration/data directories are deliberately preserved. They are ABI,
protocol, image, or upgrade compatibility identifiers—not missed branding.

## License

The Anbox Reboxed host source is GPLv3 unless a file states otherwise.
Third-party and inherited components retain their original licenses and
copyright notices.
