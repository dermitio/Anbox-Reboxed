# GSI vendor integration

The public repository contains source for the pipe compatibility library,
container power service, and bootclasspath stubs. It does not include a
vendor image, extracted HAL binaries, or an extracted system image.

For installation, supply an already compatible vendor image with
ANBOX_REBOXED_VENDOR_IMAGE. For rebuilding, extract a compatible vendor
image separately, preserve its HAL/VINTF/init/SELinux configuration, and
point the helper at that tree:

```sh
ANBOX_VENDOR_SOURCE_DIR=/path/to/extracted-vendor \
ANDROID_ROOTFS=/path/to/extracted-android-root \
ANDROID_RUNTIME_APEX=/path/to/com.android.runtime.apex \
./gsi-vendor/build-vendor-image.sh /path/to/vendor.reboxed.img 128M
```

The helper adds the project-built compatibility components to that donor
tree and creates an ext4 image. It requires Clang/LLD, binutils, e2fsprogs,
unzip, JDK 17, and Android SDK d8. The Android root must supply
system/lib64/libbinder_ndk.so and liblog.so; the runtime APEX must match
that image's ABI. Increase the output size for larger donor trees.

The donor's service configuration must match its HALs and the container.
This helper is not a generator for a complete generic vendor distribution.
No binaries are downloaded automatically. Use a disposable extracted copy:
the helper writes its generated components into the supplied tree.
The historical local defaults remain available for development, but no
local extracted tree is required for a host build.

The project-owned classpath wrapper is retained in templates. An image that
uses it must also provide matching init integration. Image-specific VINTF,
SELinux policy, and HAL configuration cannot be inferred from the host
source alone.
