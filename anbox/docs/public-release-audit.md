# Public release preparation: 0.0.1-devalpha

The public snapshot retains host/runtime sources, x86/x86_64 Android
integration, networking, desktop Bridge sources, tests, patches, build
metadata, project assets, and original license notices.

The clone check uncovered overbroad pre-existing ignore rules: build*/
hid anbox/src/anbox/build/config.h.in, and **/core hid 42 process-cpp
source/header files plus anbox/src/core/property.h. These build inputs
are now tracked. Only the generated config.h remains ignored. Three
previously untracked window-geometry source/test files required by the
current core build are also included.

## Tracking changes

The complete path inventory is in [public-release-untracked.txt](public-release-untracked.txt).
All 1,172 listed local files are preserved. It includes the Android Berberis and
native_bridge_support imports, their development documentation, ARM64
fixtures/tests, rebuild/source-preparation scripts, product fragment, and
dependency lock. Obsolete nested GitHub/Travis automation and submodule
metadata are also retained locally but excluded from publication.
The machine-specific USER0-RECOVERY note and obsolete GSI BOOT_STATUS
report are excluded as private development records.

The ARM test CMake block is preserved locally as BerberisLocal.cmake,
behind an explicit default-OFF option. The index from before cleanup is
backed up under Git's private release-preparation-backup directory.

Ignore rules exclude those exact development paths, alternate root build
directories, local ISA notes, runtime test leftovers, crash dumps, image
formats, APK signing sidecars, and private signing keystores. Existing
rules already exclude extracted images, proprietary translator downloads,
research trees, build products, logs, caches, and local editor state.
No general project source directory is ignored.

## Dependencies and retained material

See [public-build.md](public-build.md). The installer composes the Bridge
without requiring Berberis; translation remains an explicit local opt-in.
The x86_64 Android product uses the same opt-in. The vendor helper accepts
ANBOX_VENDOR_SOURCE_DIR and documents the required extracted donor tree.
Staged installation also exposed an unconditional install rule for the
already-ignored GLES replacement bundle. That rule now accepts an optional
external ANBOX_GLES_COMPAT_DIR, with installer forwarding and explicit
runtime diagnostics when the image-specific bundle is absent.

Generic Native Bridge management and its synthetic tests remain because
runtime diagnostics and existing installation management use them. They
include no translator implementation or downloaded payload.

All anbox/external libraries are referenced by the build (nsexec is a
separate helper). They remain with their licenses. Legacy Android HAL,
Snap, Docker, and kernel-module integration is retained as source; its
compatibility with modern images/kernels has not been established by this
cleanup. Image-specific gfxstream compatibility hashes and patches remain
intentional compatibility metadata, not development-machine configuration.
Artwork is retained as supplied; authorship was not independently verified.

## Security and history

Tracked content and the reachable initial commit were checked for private
key blocks, common credential/token signatures, credential filenames,
embedded authenticated URLs, machine home paths, large blobs, and symlinks.
No credential requiring rotation was identified. APN passwords are
inherited public carrier settings; probe tokens are generated test
identifiers. This is a pattern-based audit,
not a guarantee that no arbitrary secret exists.

The only tracked symlink is anbox-modules/debian/udev, which resolves to
the tracked ../99-anbox.rules within the repository.

History still contains the excluded translation imports. The initial
commit contains four distinct 14–15 MB generated API inventory blobs
(two architecture paths share a blob). No tracked image/APK/proprietary
translator binary was identified. Because the release policy excludes
unready Berberis from publication, publishing the current history would
still expose it. Before a public push, consider an explicitly approved
history filter removing the paths in public-release-untracked.txt, or a
new publication history from the reviewed public snapshot. This also
removes the unnecessary generated API inventories. No history rewrite,
commit, remote change, or push was performed during this cleanup.

History also contains anbox/data/USER0-RECOVERY.md, a private recovery
record with a personal backup path and synthetic-password handle/alias.
These are not a recovered private key, but the note is not intended for
public distribution. Removing this historical path before publication is
recommended independently of the translation-tree cleanup. The obsolete
gsi-vendor/BOOT_STATUS.md is included in the removal inventory as well.

The root license is GPL-3.0-only based on inherited source grants, not
an inferred "or-later" grant. See ../../THIRD_PARTY_NOTICES.md for the
component license evidence.

## Added ignore patterns

Relative to the initial commit (the rebuild-kit entry was already a local
edit before this pass), the root .gitignore additions are listed below.
The final two lines belong to anbox/.gitignore. The old unanchored core
and build-directory rules were narrowed so source is not hidden.

```gitignore
/berberis-rebuild-kit/
/third_party/android/berberis/
/third_party/android/native_bridge_support/
/third_party/android/README.md
/third_party/android/LICENSE.Apache-2.0
/anbox/tests/selftests/BerberisLocal.cmake
/anbox/tests/selftests/berberis_arm64_*
/anbox/tests/selftests/arm64_elf/
/anbox/tests/selftests/translation_sources_selftest.py
/anbox/scripts/build-berberis-arm64-standalone
/anbox/scripts/build-arm64-*-standalone
/anbox/scripts/check-berberis-rebuild-kit
/anbox/scripts/create-berberis-rebuild-kit
/anbox/scripts/prepare-android-translation-sources
/anbox/products/anbox_berberis_arm64.mk
/anbox/data/native-bridge/berberis-dependencies.lock.json
/anbox/docs/berberis-rebuild-kit.md
/anbox/docs/native-bridge-berberis.md
/anbox/android/native-bridge-vulkan-probe/
/ISA_Family_checklist.md
/berberis-arm64-runtime-*
/.local-development/
/anbox/.gitmodules
/anbox/.travis.yml
/anbox/.github/
/build-*/
/core
/core.*
**/*.core
**/*.img
**/*.qcow2
**/*.vmdk
**/*.iso
**/*.apk.idsig
**/*.keystore
**/*.jks
**/*.pk8
/leisuretest.txt
/anbox/data/USER0-RECOVERY.md
/gsi-vendor/BOOT_STATUS.md
/build*/
/src/anbox/build/config.h
```

## Validation

A separate build-public/source copy containing only tracked working-tree
files configured and built successfully with GCC, Werror=ON, and the
separately supplied gfxstream backend plus matching render-utils headers.
The original local build and Android images were not replaced.

All 11 registered CTest tests passed: xdg, input pipeline, viewport,
window geometry, public installer preflight, gfxstream compatibility and
patch checks, Native Bridge manager, data image, signature spoof, and
Reboxed Bridge. The installer preflight test confirms that default image
composition does not need a Berberis descriptor and that explicit local
opt-in reports a missing descriptor. A test invocation was corrected to
place the optional Native Bridge backend before its options.

The built executable reports 0.0.1-devalpha. Shell syntax and both staged
and unstaged git diff --check pass. Removed paths were checked against
the index and filesystem: all 1,172 local copies remain and none is tracked.
No remaining tracked file exceeds 1 MB, and no machine-home path or
broken/external symlink was found in the final tree.

DESTDIR installation into build-public/install-stage also passed, without
the ignored GLES payload and without changing the host installation.
The complete 11-test suite was rerun successfully after that install fix.

GMock/GTest development packages were not available to CMake, so the
optional inherited unit-test targets were not registered; they are not
reported as passed. Android probe APKs were explicitly disabled for the
host build. No new Android image boot, privileged host installation,
or end-to-end x86 application/networking session was performed. Those
require the intended compatible system/vendor images, platform-signed
Bridge APK, and a runtime session.
