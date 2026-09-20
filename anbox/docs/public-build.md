# Building 0.0.1-devalpha from a public checkout

The public tree contains the Reboxed host runtime, x86/x86_64 Android
integration, networking, desktop Bridge sources, compatibility patches,
and project tests. ARM translation is not part of this release.

## Host build

Install CMake, a C/C++17 compiler, make or Ninja, pkg-config, Python 3,
Clang/LLD (including support for the Android cross-target compatibility
libraries), and development packages for Boost filesystem/log/serialization/
thread/program_options, Protobuf (including protoc), LXC, SDL2, SDL2_image,
EGL, GLES2, X11, D-Bus, systemd, libcap, and GLM. GTest/GMock are optional
for the inherited unit tests. Vendored libraries in anbox/external are
required build inputs and are included with their original notices.

From the repository root:

```sh
cmake -S anbox -B build -DANBOX_BUILD_ANDROID_PROBES=OFF
cmake --build build -j2
ctest --test-dir build --output-on-failure
build/src/anbox-reboxed version
```

The default version is 0.0.1-devalpha. Android probe APKs are optional;
enable ANBOX_BUILD_ANDROID_PROBES and set ANBOX_ANDROID_SDK_ROOT to an
Android SDK with platforms/build-tools to build them. They are test inputs,
not the platform-signed Reboxed Bridge APK.

Modern Android graphics additionally needs a separately obtained gfxstream
backend and matching render-utils headers. See [renderer.md](renderer.md)
and the retained patch in anbox/patches. Pass GFXSTREAM_BACKEND_LIBRARY and
GFXSTREAM_RENDER_UTILS_INCLUDE_DIR to CMake. The installer accepts
ANBOX_REBOXED_GFXSTREAM_LIBRARY and ANBOX_REBOXED_GFXSTREAM_INCLUDE_DIR.
Without matching development headers, CMake explicitly disables modern
transport; a successful host build alone does not validate Android 15
graphics or boot compatibility. The installer requires the modern backend.

The image-specific GLES guest replacement bundle is also external. CMake
accepts ANBOX_GLES_COMPAT_DIR for a directory containing the layout in
anbox/data/android-gles-compat-v1.json (including
android-15.0.0_r26/x86_64 libraries). The manifest records the gfxstream
and goldfish-opengl source revisions, library names, build IDs, and hashes.
Supply a matching built bundle for images requiring this workaround;
neither the source checkout nor CMake downloads it. Without the bundle,
host installation succeeds but the GLES replacement operation reports
the missing payload. This is an image compatibility prerequisite, not
proof that arbitrary Android images will boot.
The installer forwards ANBOX_REBOXED_GLES_COMPAT_DIR to CMake; the
compatibility tool itself accepts ANBOX_GLES_COMPAT_REPLACEMENTS.

## Images and the desktop Bridge

Supply compatible x86/x86_64 system and vendor images separately. No images,
extracted root filesystems, APK payloads, signing keys, SDKs, or downloaded
renderer source/binaries are distributed in this repository.

Build ReboxedBridge from anbox/android/reboxed-bridge in a matching AOSP
tree using its Android.bp module and the image's platform signing setup.
The Android integration lives in anbox/products. See
[reboxed-bridge.md](reboxed-bridge.md) for permissions and preload policy.
Signing material must stay outside Git; an arbitrary debug key will not
satisfy the preload verifier.

```sh
ANBOX_REBOXED_ANDROID_IMAGE=/path/to/system.img \
ANBOX_REBOXED_VENDOR_IMAGE=/path/to/vendor.img \
ANBOX_REBOXED_BRIDGE_APK=/path/to/ReboxedBridge.apk \
ANBOX_REBOXED_GFXSTREAM_LIBRARY=/path/to/libgfxstream_backend.so \
ANBOX_REBOXED_GFXSTREAM_INCLUDE_DIR=/path/to/gfxstream/include \
./anbox-reboxed-install.sh --install
```

The installer preserves the input system image and creates a composed copy.
It does not download images or keys. The optional full-AOSP action requires
an already synced and configured AOSP tree via ANBOX_REBOXED_ANDROID_BUILD_TOP.
The vendor-image helper requires an extracted compatible donor tree; see
[../../gsi-vendor/README.md](../../gsi-vendor/README.md).

Running the container also requires kernel binder/binderfs support, LXC,
loop/ext4 support, systemd integration, networking tools, and permissions
described in [install.md](install.md). Host tests do not replace a boot test
with the intended system/vendor image pair.

## Local unpublished development

ANBOX_BUILD_LOCAL_BERBERIS_TESTS defaults OFF and fails clearly if requested
without the local test include. ANBOX_REBOXED_ENABLE_LOCAL_BERBERIS=1
and ANBOX_REBOXED_BERBERIS_ARTIFACTS opt the installer into a separately
preserved local translation workflow. Public users should leave these
disabled. No source fetch or translation payload is implied.
