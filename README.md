# Anbox Reboxed

Version: **0.0.1-devalpha**

Development is slow, irregular, and done in spare time; expect long pauses
and unfinished features. There is no release schedule or support guarantee.
Contributions will not be accepted until Reboxed is out of devalpha.
See [contributers.md](contributers.md).

Anbox Reboxed is an unofficial, AI-assisted revival fork of the archived open-source Anbox project.

Its goal is to modernize Anbox for newer Linux kernels, improve compatibility with modern Android GSI images, and make the project usable again for personal desktop Android-container experimentation.

This project is not affiliated with, endorsed by, or maintained by Canonical, the original Anbox authors, or the Anbox Cloud project.

The combined Reboxed project is licensed under **GPL-3.0-only**, following
the inherited Anbox core's explicit version-3 grant. Project-owned additions
use that license unless explicitly stated otherwise. Existing per-file and
third-party licenses remain intact see [LICENSE](LICENSE) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Why?

Anbox Reboxed exists because fixing Waydroid would have been too normal.

It started with one question:

Do I really want to revive an archived Android container project just to play one game?

*Probably not.*

Am I going to do so anyway?

**Yes, absolutely.**

...
And then I decided it would be a public project instead of a personal thing.

## Project status

Anbox Reboxed currently boots Android 15, launches Android applications, provides network connectivity, and includes revised rendering, hardware-acceleration, input, debugging, and desktop-integration features.

This remains an experimental project. It has primarily been tested on one machine with one Android 15 image. Other systems and GSI images may require additional configuration or source changes.

This public snapshot supports native x86/x86_64 Android applications.
Unfinished Berberis ARM/native translation is excluded from the public tree.

Users must supply compatible Android system/vendor images and a Reboxed
Bridge APK signed with that image's platform key. Images, signing keys,
downloaded renderers, and SDKs are not bundled. See
[the public build guide](anbox/docs/public-build.md) for a fresh checkout,
external prerequisites, and tests.

## Install and update

Anbox Reboxed includes a transactional host installer that preserves Android images and user data while recording file hashes and timestamped rollback copies.

```sh
./anbox-reboxed-install.sh --install
./anbox-reboxed-install.sh --update
./anbox-reboxed-install.sh --verify
./anbox-reboxed-install.sh --rollback
./anbox-reboxed-install.sh --uninstall
```

The installer derives the checkout and source paths from its own location,
uses the checkout's single `build` directory, and runs two parallel build jobs
by default. Portable bundles can override discovery with
`ANBOX_REBOXED_SOURCE_DIR`, `ANBOX_REBOXED_BUILD_DIR`,
`ANBOX_REBOXED_ANDROID_IMAGE`, and `ANBOX_REBOXED_VENDOR_IMAGE`.

See [the installation guide](anbox/docs/install.md) for build requirements, installed paths, verification behavior, launch options, update migration, and recovery details.

## Desktop and orientation

The application menu provides three launchers:

* `Anbox Reboxed`
* `Anbox Reboxed — Portrait`
* `Anbox Reboxed — Landscape`

The launcher selects a framebuffer profile that fits the available desktop work area. Provided portrait profiles include:

* `720×1280`
* `540×960`
* `360×640`

Landscape mode uses the corresponding rotated dimensions.

Native framebuffer orientation is used instead of window-manager rotation, keeping rendering and mouse or touch coordinates aligned.

The recommended modern-phone Android profile is:

```text
720×1600 at 320 DPI
```

This produces a logical Android display of approximately `360×800 dp` with a 20:9 aspect ratio.

On a desktop with 1048 usable vertical pixels, that framebuffer can be scaled to approximately `472×1048` while preserving its aspect ratio. This is the host presentation size, not the Android framebuffer resolution.

```sh
anbox-reboxed display-status

anbox-reboxed-window \
  --orientation portrait \
  --width 720 \
  --height 1600 \
  --save

anbox-reboxed-window --orientation landscape
```

Run the following to see all available launch, renderer, input, display, QoL, and debugging options:

```sh
anbox-reboxed --help
```
