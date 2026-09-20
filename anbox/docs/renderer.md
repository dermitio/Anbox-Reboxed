# Renderer and acceleration

The default presentation backend is `reboxed`. The original synchronous path
remains available temporarily with `--renderer=legacy`.

For the reference X11 profile, start the session manager with:

```text
anbox-reboxed session-manager --single-window --android-size=720x1600 --density=320
```

`--android-size` controls Android's framebuffer and logical display metrics.
`--density` controls Android UI density. `--window-size` controls only the
initial host window. Resizing the host window does not change Android's logical
resolution; the renderer preserves aspect ratio and letterboxes as necessary.

After create, maximize, restore, minimize, show, move, monitor change, or
compositor resize, input mapping is invalidated. The next presentation queries
the real EGL drawable, applies `glViewport`, then publishes window size,
drawable size, rendered offset/extent, Android size, rotation, and a monotonic
generation. This publication is the only pointer-mapping authority.

## Modern Android 15 transport

`--graphics-api=vulkan` selects gfxstream and advertises GLES 3.0 to Android.
`auto` selects the same path when gfxstream support was compiled in; `gles`
keeps the original GLES2 encoder/decoder path for comparison. The modern data
path is:

```text
Android EGL/HWUI/WebView
  -> ranchu qemu-pipe encoder
  -> bounded host gfxstream RenderChannel
  -> host GLES/Vulkan driver
  -> asynchronous PBO readback drain
  -> newest-only RGBA presentation frame
  -> aspect-preserving host EGL viewport
```

The post callback in an asynchronous gfxstream build is a readiness signal,
not a populated CPU buffer. Anbox Reboxed calls gfxstream's read-pixels
function before publishing the frame. Skipping that drain produces valid frame
notifications containing all-zero pixels, which looks like intermittent black
or white application windows. The presenter copies one completed frame and
replaces the prior pending frame, so guest stalls cannot create an unbounded
backlog.

The Android 15 ranchu mapper shipped with the supported vendor image calls an
obsolete DMA-only ColorBuffer update slot. `anbox-gfxstream-compat` validates
the mapper's architecture, Build ID, complete input hash, changed instruction,
and complete output hash, then creates a read-only overlay. It never modifies
`vendor.img`; unknown vendor builds fail closed and remove stale generated
overlays. Diagnose it with:

```text
anbox-reboxed gfxstream-compatibility-status
```

gfxstream protocol compatibility is source-sensitive even though distributions
often publish every snapshot as library version `0.1.2`. The installer uses
the current workspace's configured backend and `render-utils` headers, then
installs that backend privately under `/usr/local/lib/anbox-reboxed`; the
executable's relative RUNPATH selects it without replacing the distribution
library. A deployment may set `ANBOX_REBOXED_GFXSTREAM_EXPECTED_REVISION` to
enforce a revision policy, but the installer does not fetch or require a
historical checkout when the current workspace configures and tests cleanly.

Older gfxstream trees needed a ColorBuffer synchronization and GLES translator
patch. The installer now detects the relevant capabilities and only considers
that patch when its original source layout is present. Current implementations
that already synchronize ColorBuffers or no longer use that GLES translator
are reported as compatible/obsolete-patch layouts rather than treated as a
patch failure.

Packagers can provide an already-built matching pair with
`ANBOX_REBOXED_GFXSTREAM_LIBRARY` and
`ANBOX_REBOXED_GFXSTREAM_INCLUDE_DIR`. Direct CMake builds use
`GFXSTREAM_BACKEND_LIBRARY`, `GFXSTREAM_RENDER_UTILS_INCLUDE_DIR`, and
`GFXSTREAM_PROTOCOL_REVISION`. The startup diagnostic prints both the expected
protocol revision and the loaded library path. If private render-utils headers
are absent, CMake disables the modern transport explicitly.

## Acceleration diagnostics

At startup the renderer logs the EGL vendor/version/client APIs and the GL
vendor/renderer/version. Known software rasterizers (`llvmpipe`, `softpipe`,
`swrast`, Mesa's software rasterizer, and SwiftShader) are classified as
software. A host-driver request fails instead of silently accepting one of
those implementations. Use `--software-rendering` for an intentional
SwiftShader session, or `--allow-software-fallback` to explicitly accept a
host-selected software renderer.

For a modern session the log additionally prints `Graphics
transport=gfxstream`, GLES vendor/renderer/version, whether Vulkan is enabled,
readback mode, and queue policy. gfxstream prints the selected Vulkan physical
device. Treat `llvmpipe`, `softpipe`, `swrast`, or SwiftShader in either set of
diagnostics as software acceleration.

The log also reports the selected backend, requested driver, acceleration
classification, and presentation queue capacity. The revised queue has one
pending slot: if presentation stalls, a new frame replaces the stale pending
frame. One last-presented frame is retained separately so host lifecycle
events can repaint immediately without waiting for SurfaceFlinger. Redraw
requests coalesce to one flag, so this remains bounded at one pending plus one
last frame. ColorBuffers are released when either ownership slot is replaced.

## Ownership and synchronization

The guest sends emulated render-control handles, not dma-buf or native-fence
file descriptors. Guest EGL fences currently complete through a synchronous
host `glFinish`; there is therefore no fence FD for the host presenter to own.
Each guest ColorBuffer owns its host textures/EGLImages. Each queued frame owns
a temporary reference to every ColorBuffer it names. The renderer owns guest
contexts and pbuffers, host EGL contexts, and host window EGLSurfaces. An SDL
window owns the native X11/Wayland window borrowed by its EGLSurface, so the
EGLSurface is destroyed first. The presentation worker stops before EGL
finalization.

## Current transport path

Android gralloc and EGL encode GLES/render-control packets over the qemu-pipe
socket. Host render threads decode those packets into shared host textures.
`rcFBPost` or the layer-post protocol creates renderables, the layer strategy
maps them to host windows, and the selected presentation backend submits them
to the emulator renderer. The revised backend moves host presentation off the
guest decoder thread and keeps only the newest pending frame; the legacy
backend invokes presentation synchronously.
