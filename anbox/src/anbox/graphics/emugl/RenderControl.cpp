/*
* Copyright (C) 2011 The Android Open Source Project
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include "anbox/graphics/emugl/RenderControl.h"
#include "anbox/graphics/emugl/DispatchTables.h"
#include "anbox/graphics/emugl/DisplayManager.h"
#include "anbox/graphics/emugl/RenderThreadInfo.h"
#include "anbox/graphics/emugl/Renderer.h"
#include "anbox/graphics/emugl/RendererConfig.h"
#include "anbox/graphics/layer_composer.h"
#include "anbox/logger.h"

#include "external/android-emugl/shared/OpenglCodecCommon/ChecksumCalculatorThreadInfo.h"
#include "external/android-emugl/host/include/OpenGLESDispatch/EGLDispatch.h"

#include <map>
#include <string>
#include <sstream>

static const GLint rendererVersion = 1;
static std::shared_ptr<anbox::graphics::LayerComposer> composer;
static std::shared_ptr<Renderer> renderer;

void registerLayerComposer(
    const std::shared_ptr<anbox::graphics::LayerComposer> &c) {
  composer = c;
}

void registerRenderer(const std::shared_ptr<Renderer> &r) {
  renderer = r;
}

static GLint rcGetRendererVersion() { return rendererVersion; }

static EGLint rcGetEGLVersion(EGLint *major, EGLint *minor) {
  if (!renderer)
    return EGL_FALSE;

  *major = static_cast<EGLint>(renderer->getCaps().eglMajor);
  *minor = static_cast<EGLint>(renderer->getCaps().eglMinor);

  return EGL_TRUE;
}

static std::string filter_extensions(const std::string& extensions, const std::vector<std::string>& whitelist) {
  std::stringstream approved_extensions;
  auto extension_list = anbox::utils::string_split(extensions, ' ');
  for (const auto& ext : extension_list) {
    if (std::find(whitelist.begin(), whitelist.end(), ext) == whitelist.end())
      continue;

    if (approved_extensions.tellp() > 0)
      approved_extensions << " ";

    approved_extensions << ext;
  }
  return approved_extensions.str();
}

static void append_extension(std::string& extensions,
                             const std::string& extension) {
  const auto extension_list = anbox::utils::string_split(extensions, ' ');
  if (std::find(extension_list.begin(), extension_list.end(), extension) !=
      extension_list.end())
    return;
  if (!extensions.empty())
    extensions += " ";
  extensions += extension;
}

static EGLint rcQueryEGLString(EGLenum name, void* buffer, EGLint bufferSize) {
  if (!renderer)
    return 0;

  std::string result = s_egl.eglQueryString(renderer->getDisplay(), name);
  if (result.empty())
    return 0;

  if (name == EGL_EXTENSIONS) {
    // We need to drop a few extensions from the list reported by the driver
    // as not all are well enough support by our EGL implementation.
    std::vector<std::string> whitelisted_extensions = {
      "EGL_KHR_image_base",
      "EGL_KHR_gl_texture_2D_image",
    };
    result = filter_extensions(result, whitelisted_extensions);
  }

  int len = result.length() + 1;
  if (!buffer || len > bufferSize) {
    return -len;
  }

  strcpy(static_cast<char*>(buffer), result.c_str());
  return len;
}

static EGLint rcGetGLString(EGLenum name, void* buffer, EGLint bufferSize) {
  RenderThreadInfo* tInfo = RenderThreadInfo::get();
  std::string result;

  if (tInfo && tInfo->currContext) {
    const char* str = nullptr;
    if (tInfo->currContext->isGL2())
      str = reinterpret_cast<const char*>(s_gles2.glGetString(name));
    else
      str = reinterpret_cast<const char*>(s_gles1.glGetString(name));

    if (str)
      result += str;
  }

  // The legacy renderer decodes the GLES 2 command stream.  Guest EGL may
  // still request an ES 3 context (Android 15 does so unconditionally), and
  // the backing Mesa context is consequently ES 3, but forwarding Mesa's
  // version strings would advertise a GLES 3 interface that this decoder does
  // not implement.  In particular Skia would then enumerate GLES 3
  // extensions with glGetStringi(), while the legacy guest/host codec only
  // provides the GLES 2 extension string.  Keep the GL and GLSL identities
  // consistent with the command interface actually exposed to the guest.
  if (name == GL_VERSION)
    result = "OpenGL ES 2.0 Anbox Reboxed compatibility";
  else if (name == GL_SHADING_LANGUAGE_VERSION)
    result = "OpenGL ES GLSL ES 1.00 Anbox Reboxed compatibility";
  else if (name == GL_EXTENSIONS) {
    // We need to drop a few extensions from the list reported by the driver
    // as not all are well enough support by our GL implementation.
    std::vector<std::string> whitelisted_extensions = {
      "GL_OES_EGL_image",
      "GL_OES_depth24",
      "GL_OES_depth32",
      "GL_OES_element_index_uint",
      "GL_OES_texture_float",
      "GL_OES_texture_float_linear",
      "GL_OES_compressed_paletted_texture",
      "GL_OES_compressed_ETC1_RGB8_texture",
      "GL_OES_depth_texture",
      "GL_OES_texture_half_float",
      "GL_OES_texture_half_float_linear",
      "GL_OES_packed_depth_stencil",
      "GL_OES_vertex_half_float",
      "GL_OES_standard_derivatives",
      "GL_OES_texture_npot",
      "GL_OES_rgb8_rgba8",
    };

    result = filter_extensions(result, whitelisted_extensions);
    // The guest encoder implements external textures on top of the host
    // ColorBuffer's GL_TEXTURE_2D image and rewrites samplerExternalOES to
    // sampler2D.
    append_extension(result, "GL_OES_EGL_image_external");
    // Modern emulator EGL derives its client-side extension table from this
    // legacy renderControl reply as well as rcGetHostExtensionsString.  Keep
    // the maximum guest context version visible here so it retains the
    // external-image capability when constructing an Android 15 ES3 context;
    // the GL/GLSL identity above remains GLES2 because that is the decoded
    // command interface.
    append_extension(result, "ANDROID_EMU_gles_max_version_3_0");
  }

  int nextBufferSize = result.size() + 1;

  if (!buffer || nextBufferSize > bufferSize)
    return -nextBufferSize;

  snprintf(static_cast<char*>(buffer), nextBufferSize, "%s", result.c_str());
  return nextBufferSize;
}

static EGLint rcGetNumConfigs(uint32_t *p_numAttribs) {
  int numConfigs = 0, numAttribs = 0;

  renderer->getConfigs()->getPackInfo(&numConfigs, &numAttribs);
  if (p_numAttribs) {
    *p_numAttribs = static_cast<uint32_t>(numAttribs);
  }
  return numConfigs;
}

static EGLint rcGetConfigs(uint32_t bufSize, GLuint *buffer) {
  GLuint bufferSize = static_cast<GLuint>(bufSize);
  return renderer->getConfigs()->packConfigs(bufferSize, buffer);
}

static EGLint rcChooseConfig(EGLint *attribs, uint32_t attribs_size,
                             uint32_t *configs, uint32_t configs_size) {
  if (!renderer || attribs_size == 0)
    return 0;

  return renderer->getConfigs()->chooseConfig(attribs, reinterpret_cast<EGLint *>(configs),
                                              static_cast<EGLint>(configs_size));
}

static EGLint rcGetFBParam(EGLint param) {
  if (!renderer)
    return 0;

  EGLint ret = 0;

  switch (param) {
    case FB_WIDTH:
      ret = static_cast<EGLint>(anbox::graphics::emugl::DisplayInfo::get()->width());
      break;
    case FB_HEIGHT:
      ret = static_cast<EGLint>(anbox::graphics::emugl::DisplayInfo::get()->height());
      break;
    case FB_XDPI:
    case FB_YDPI:
      ret = static_cast<EGLint>(anbox::graphics::emugl::DisplayInfo::get()->density());
      break;
    case FB_FPS:
      ret = 60;
      break;
    case FB_MIN_SWAP_INTERVAL:
      ret = 1;  // XXX: should be implemented
      break;
    case FB_MAX_SWAP_INTERVAL:
      ret = 1;  // XXX: should be implemented
      break;
    default:
      break;
  }

  return ret;
}

static uint32_t rcCreateContext(uint32_t config, uint32_t share,
                                uint32_t glVersion) {
  if (!renderer)
    return 0;

  HandleType ret = renderer->createRenderContext(config, share, glVersion);
  WARNING("Context lifecycle: create config=%u share=%#x guest_gles=%u host_decoder=%s result=%#x",
          config, share, glVersion,
          glVersion == 2 || glVersion == 3 ? "gles2" : "gles1", ret);
  return ret;
}

static void rcDestroyContext(uint32_t context) {
  if (!renderer)
    return;

  renderer->DestroyRenderContext(context);
}

static uint32_t rcCreateWindowSurface(uint32_t config, uint32_t width,
                                      uint32_t height) {
  if (!renderer)
    return 0;

  const auto handle = renderer->createWindowSurface(config, width, height);
  DEBUG("rcCreateWindowSurface config=%u size=%ux%u result=%#x", config,
        width, height, handle);
  return handle;
}

static void rcDestroyWindowSurface(uint32_t windowSurface) {
  if (!renderer)
    return;

  renderer->DestroyWindowSurface(windowSurface);
}

static uint32_t rcCreateColorBuffer(uint32_t width, uint32_t height,
                                    GLenum internalFormat) {
  if (!renderer)
    return 0;

  return renderer->createColorBuffer(width, height, internalFormat);
}

static int rcOpenColorBuffer2(uint32_t colorbuffer) {
  if (!renderer)
    return -1;

  return renderer->openColorBuffer(colorbuffer);
}

// Deprecated, kept for compatibility with old system images only.
// Use rcOpenColorBuffer2 instead.
static void rcOpenColorBuffer(uint32_t colorbuffer) {
  (void)rcOpenColorBuffer2(colorbuffer);
}

static void rcCloseColorBuffer(uint32_t colorbuffer) {
  if (!renderer)
    return;

  renderer->closeColorBuffer(colorbuffer);
}

static int rcFlushWindowColorBuffer(uint32_t windowSurface) {
  if (!renderer)
    return -1;

  if (!renderer->flushWindowSurfaceColorBuffer(windowSurface))
    return -1;

  return 0;
}

static void rcSetWindowColorBuffer(uint32_t windowSurface,
                                   uint32_t colorBuffer) {
  if (!renderer)
    return;

  renderer->setWindowSurfaceColorBuffer(windowSurface, colorBuffer);
}

static EGLint rcMakeCurrent(uint32_t context, uint32_t drawSurf,
                            uint32_t readSurf) {
  if (!renderer)
    return EGL_FALSE;

  bool ret = renderer->bindContext(context, drawSurf, readSurf);
  if (!ret)
    WARNING("Presentation trace: rcMakeCurrent failed context=%#x draw=%#x read=%#x",
            context, drawSurf, readSurf);

  return (ret ? EGL_TRUE : EGL_FALSE);
}

static void rcFBPost(uint32_t colorBuffer) {
  int width = 0;
  int height = 0;
  if (!renderer || !composer ||
      !renderer->retainPostedColorBuffer(colorBuffer, &width, &height))
    return;
  static unsigned int post_trace_counter = 0;
  if ((++post_trace_counter % 60) == 1)
    WARNING("Presentation trace: rcFBPost color_buffer=%#x size=%dx%d", colorBuffer,
            width, height);
  composer->submit_layers({Renderable{"org.anbox.primary-display", colorBuffer,
      1.0f, {0, 0, width, height}, {0, 0, width, height}}});
}

static void rcFBSetSwapInterval(EGLint) {
  // XXX: TBD - should be implemented
}

static void rcBindTexture(uint32_t colorBuffer) {
  if (!renderer)
    return;

  if (!renderer->bindColorBufferToTexture(colorBuffer))
    WARNING("ColorBuffer texture import failed: handle=%#x", colorBuffer);
}

static void rcBindRenderbuffer(uint32_t colorBuffer) {
  if (!renderer)
    return;

  renderer->bindColorBufferToRenderbuffer(colorBuffer);
}

static EGLint rcColorBufferCacheFlush(uint32_t, EGLint, 
                                      int) {
  // XXX: TBD - should be implemented
  return 0;
}

static void rcReadColorBuffer(uint32_t colorBuffer, GLint x, GLint y,
                              GLint width, GLint height, GLenum format,
                              GLenum type, void *pixels) {
  if (!renderer)
    return;

  renderer->readColorBuffer(colorBuffer, x, y, width, height, format, type, pixels);
}

static int rcUpdateColorBuffer(uint32_t colorBuffer, GLint x, GLint y,
                               GLint width, GLint height, GLenum format,
                               GLenum type, void *pixels) {
  if (!renderer)
    return -1;

  renderer->updateColorBuffer(colorBuffer, x, y, width, height, format, type, pixels);
  return 0;
}

static uint32_t rcCreateClientImage(uint32_t context, EGLenum target,
                                    GLuint buffer) {
  if (!renderer)
    return 0;

  return renderer->createClientImage(context, target, buffer);
}

static int rcDestroyClientImage(uint32_t image) {
  if (!renderer)
    return 0;

  return renderer->destroyClientImage(image);
}

static void rcSelectChecksumCalculator(uint32_t protocol, uint32_t) {
  ChecksumCalculatorThreadInfo::setVersion(protocol);
}

int rcGetNumDisplays() {
  // For now we only support a single display but that single display
  // will contain more than one display so that we simply spawn up a big
  // virtual display which should match the real display arrangement
  // in most cases.
  return 1;
}

int rcGetDisplayWidth(uint32_t display_id) {
  (void)display_id;
  return static_cast<int>(anbox::graphics::emugl::DisplayInfo::get()->width());
}

int rcGetDisplayHeight(uint32_t display_id) {
  (void)display_id;
  return static_cast<int>(anbox::graphics::emugl::DisplayInfo::get()->height());
}

int rcGetDisplayDpiX(uint32_t display_id) {
  (void)display_id;
  return static_cast<int>(anbox::graphics::emugl::DisplayInfo::get()->density());
}

int rcGetDisplayDpiY(uint32_t display_id) {
  (void)display_id;
  return static_cast<int>(anbox::graphics::emugl::DisplayInfo::get()->density());
}

int rcGetDisplayVsyncPeriod(uint32_t display_id) {
  (void)display_id;
  return 16666666;  // nanoseconds at 60 Hz
}

// Layer batches are produced by a guest render-control connection. They must
// not be shared between decoder threads or one client can finish another
// client's partially assembled frame.
static thread_local std::vector<Renderable> frame_layers;

bool is_layer_blacklisted(const std::string &name) {
  static std::vector<std::string> blacklist = {
      // The 'Sprite' layer is the mouse cursor Android uses as soon
      // as it has a pointer input device available. We don't want to
      // display this layer at all but don't have a good way of disabling
      // the cursor on the Android side yet.
      "Sprite",
  };
  return std::find(blacklist.begin(), blacklist.end(), name) != blacklist.end();
}

void rcPostLayer(const char *name, uint32_t color_buffer, float alpha,
                 int32_t sourceCropLeft, int32_t sourceCropTop,
                 int32_t sourceCropRight, int32_t sourceCropBottom,
                 int32_t displayFrameLeft, int32_t displayFrameTop,
                 int32_t displayFrameRight, int32_t displayFrameBottom) {
  Renderable r{
      name,
      color_buffer,
      alpha,
      {displayFrameLeft, displayFrameTop, displayFrameRight, displayFrameBottom},
      {sourceCropLeft, sourceCropTop, sourceCropRight, sourceCropBottom}};
  frame_layers.push_back(r);
}

void rcPostAllLayersDone() {
  if (composer) composer->submit_layers(frame_layers);

  frame_layers.clear();
}

void initRenderControlContext(renderControl_decoder_context_t *dec) {
  dec->rcGetRendererVersion = rcGetRendererVersion;
  dec->rcGetEGLVersion = rcGetEGLVersion;
  dec->rcQueryEGLString = rcQueryEGLString;
  dec->rcGetGLString = rcGetGLString;
  dec->rcGetNumConfigs = rcGetNumConfigs;
  dec->rcGetConfigs = rcGetConfigs;
  dec->rcChooseConfig = rcChooseConfig;
  dec->rcGetFBParam = rcGetFBParam;
  dec->rcCreateContext = rcCreateContext;
  dec->rcDestroyContext = rcDestroyContext;
  dec->rcCreateWindowSurface = rcCreateWindowSurface;
  dec->rcDestroyWindowSurface = rcDestroyWindowSurface;
  dec->rcCreateColorBuffer = rcCreateColorBuffer;
  dec->rcOpenColorBuffer = rcOpenColorBuffer;
  dec->rcCloseColorBuffer = rcCloseColorBuffer;
  dec->rcSetWindowColorBuffer = rcSetWindowColorBuffer;
  dec->rcFlushWindowColorBuffer = rcFlushWindowColorBuffer;
  dec->rcMakeCurrent = rcMakeCurrent;
  dec->rcFBPost = rcFBPost;
  dec->rcFBSetSwapInterval = rcFBSetSwapInterval;
  dec->rcBindTexture = rcBindTexture;
  dec->rcBindRenderbuffer = rcBindRenderbuffer;
  dec->rcColorBufferCacheFlush = rcColorBufferCacheFlush;
  dec->rcReadColorBuffer = rcReadColorBuffer;
  dec->rcUpdateColorBuffer = rcUpdateColorBuffer;
  dec->rcOpenColorBuffer2 = rcOpenColorBuffer2;
  dec->rcCreateClientImage = rcCreateClientImage;
  dec->rcDestroyClientImage = rcDestroyClientImage;
  dec->rcSelectChecksumCalculator = rcSelectChecksumCalculator;
  dec->rcGetNumDisplays = rcGetNumDisplays;
  dec->rcGetDisplayWidth = rcGetDisplayWidth;
  dec->rcGetDisplayHeight = rcGetDisplayHeight;
  dec->rcGetDisplayDpiX = rcGetDisplayDpiX;
  dec->rcGetDisplayDpiY = rcGetDisplayDpiY;
  dec->rcGetDisplayVsyncPeriod = rcGetDisplayVsyncPeriod;
  dec->rcPostLayer = rcPostLayer;
  dec->rcPostAllLayersDone = rcPostAllLayersDone;
}
