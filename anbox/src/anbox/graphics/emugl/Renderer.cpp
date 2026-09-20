/*
* Copyright (C) 2011-2015 The Android Open Source Project
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
#define GLM_ENABLE_EXPERIMENTAL
#include "anbox/graphics/emugl/Renderer.h"
#include "anbox/graphics/emugl/DispatchTables.h"
#include "anbox/graphics/emugl/RenderThreadInfo.h"
#include "anbox/graphics/emugl/TimeUtils.h"
#include "anbox/graphics/gl_extensions.h"
#include "anbox/graphics/coordinate_transform.h"
#include "anbox/graphics/opengles_message_processor.h"
#include "anbox/logger.h"

#include "external/android-emugl/host/include/OpenGLESDispatch/EGLDispatch.h"

// Generated with emugl at build time
#include "gles2_dec.h"

#include <limits>
#include <stdio.h>
#include <algorithm>
#include <cctype>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/transform.hpp>

namespace {

// Helper class to call the bind_locked() / unbind_locked() properly.
class ScopedBind {
 public:
  // Constructor will call bind_locked() on |fb|.
  // Use isValid() to check for errors.
  ScopedBind(Renderer *fb) : mFb(fb) {
    if (!fb->bind_locked()) {
      mFb = NULL;
    }
  }

  // Returns true if contruction bound the framebuffer context properly.
  bool isValid() const { return mFb != NULL; }

  // Unbound the framebuffer explictly. This is also called by the
  // destructor.
  void release() {
    if (mFb) {
      mFb->unbind_locked();
      mFb = NULL;
    }
  }

  // Destructor will call release().
  ~ScopedBind() { release(); }

 private:
  Renderer *mFb;
};

// Implementation of a ColorBuffer::Helper instance that redirects calls
// to a FrameBuffer instance.
class ColorBufferHelper : public ColorBuffer::Helper {
 public:
  ColorBufferHelper(Renderer *fb) : mFb(fb) {}

  virtual bool setupContext() { return mFb->bind_locked(); }

  virtual void teardownContext() { mFb->unbind_locked(); }

  virtual TextureDraw *getTextureDraw() const { return mFb->getTextureDraw(); }

 private:
  Renderer *mFb;
};
}  // namespace

struct RendererWindow {
  EGLNativeWindowType native_window = 0;
  EGLSurface surface = EGL_NO_SURFACE;
  anbox::graphics::Rect viewport;
  glm::mat4 screen_to_gl_coords = glm::mat4(1.0f);
  glm::mat4 display_transform = glm::mat4(1.0f);
  anbox::graphics::RenderedViewport published_viewport;
};

HandleType Renderer::s_nextHandle = 0;

void Renderer::finalize() {
  std::unique_lock<std::mutex> lock(m_lock);
  if (!m_initialized)
    return;
  s_egl.eglMakeCurrent(m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
  for (auto &entry : m_nativeWindows) {
    if (entry.second->surface != EGL_NO_SURFACE)
      s_egl.eglDestroySurface(m_eglDisplay, entry.second->surface);
    delete entry.second;
  }
  m_nativeWindows.clear();
  m_pendingWindowSizes.clear();
  m_colorbuffers.clear();
  m_colorBufferBytes = 0;
  m_windows.clear();
  m_contexts.clear();
  if (bind_locked()) {
    if (m_externalFrameTexture) {
      s_gles2.glDeleteTextures(1, &m_externalFrameTexture);
      m_externalFrameTexture = 0;
    }
    delete m_textureDraw;
    m_textureDraw = nullptr;
    m_family.clear();
    unbind_locked();
  } else {
    WARNING("Could not bind cleanup context; host presentation GL objects may leak");
  }
  if (m_eglContext != EGL_NO_CONTEXT)
    s_egl.eglDestroyContext(m_eglDisplay, m_eglContext);
  if (m_pbufContext != EGL_NO_CONTEXT)
    s_egl.eglDestroyContext(m_eglDisplay, m_pbufContext);
  if (m_pbufSurface != EGL_NO_SURFACE)
    s_egl.eglDestroySurface(m_eglDisplay, m_pbufSurface);
  s_egl.eglTerminate(m_eglDisplay);
  m_eglContext = EGL_NO_CONTEXT;
  m_pbufContext = EGL_NO_CONTEXT;
  m_pbufSurface = EGL_NO_SURFACE;
  m_eglDisplay = EGL_NO_DISPLAY;
  m_initialized = false;
}

bool Renderer::initialize(EGLNativeDisplayType nativeDisplay) {
  m_eglDisplay = s_egl.eglGetDisplay(nativeDisplay);
  if (m_eglDisplay == EGL_NO_DISPLAY) {
    ERROR("Failed to Initialize backend EGL display");
    return false;
  }

  if (!s_egl.eglInitialize(m_eglDisplay, &m_caps.eglMajor, &m_caps.eglMinor)) {
    ERROR("Failed to initialize EGL");
    return false;
  }

  anbox::graphics::GLExtensions egl_extensions{s_egl.eglQueryString(m_eglDisplay, EGL_EXTENSIONS)};

  const auto surfaceless_supported = egl_extensions.support("EGL_KHR_surfaceless_context");
  if (!surfaceless_supported)
    DEBUG("EGL doesn't support surfaceless context");

  s_egl.eglBindAPI(EGL_OPENGL_ES_API);

  // Create EGL context for framebuffer post rendering.
  GLint surfaceType = EGL_WINDOW_BIT | EGL_PBUFFER_BIT;
  const GLint configAttribs[] = {EGL_RED_SIZE, 8,
                                 EGL_GREEN_SIZE, 8,
                                 EGL_BLUE_SIZE, 8,
                                 EGL_SURFACE_TYPE, surfaceType,
                                 EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                                 EGL_NONE};

  int n;
  if ((s_egl.eglChooseConfig(m_eglDisplay, configAttribs, &m_eglConfig,
                             1, &n) == EGL_FALSE) || n == 0) {
    ERROR("Failed to select EGL configuration");
    return false;
  }

  EGLint config_id = 0;
  EGLint native_visual_id = 0;
  EGLint config_surface_type = 0;
  s_egl.eglGetConfigAttrib(m_eglDisplay, m_eglConfig, EGL_CONFIG_ID,
                           &config_id);
  s_egl.eglGetConfigAttrib(m_eglDisplay, m_eglConfig, EGL_NATIVE_VISUAL_ID,
                           &native_visual_id);
  m_nativeVisualId = native_visual_id;
  s_egl.eglGetConfigAttrib(m_eglDisplay, m_eglConfig, EGL_SURFACE_TYPE,
                           &config_surface_type);
  WARNING("Presentation trace: EGL config=%d native_visual=%#x surface_type=%#x",
          config_id, native_visual_id, config_surface_type);

  static const GLint glContextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2,
                                           EGL_NONE};

  m_eglContext = s_egl.eglCreateContext(m_eglDisplay, m_eglConfig,
                                        EGL_NO_CONTEXT, glContextAttribs);
  if (m_eglContext == EGL_NO_CONTEXT) {
    ERROR("Failed to create context: error=0x%x", s_egl.eglGetError());
    return false;
  }

  // Create another context which shares with the eglContext to be used
  // when we bind the pbuffer. That prevent switching drawable binding
  // back and forth on framebuffer context.
  // The main purpose of it is to solve a "blanking" behaviour we see on
  // on Mac platform when switching binded drawable for a context however
  // it is more efficient on other platforms as well.
  m_pbufContext = s_egl.eglCreateContext(m_eglDisplay, m_eglConfig, m_eglContext, glContextAttribs);
  if (m_pbufContext == EGL_NO_CONTEXT) {
    ERROR("Failed to create pbuffer context: error=0x%x", s_egl.eglGetError());
    return false;
  }

  if (!surfaceless_supported) {
    // Create a 1x1 pbuffer surface which will be used for binding
    // the FB context. The FB output will go to a subwindow, if one exist.
    static const EGLint pbufAttribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};

    m_pbufSurface = s_egl.eglCreatePbufferSurface(m_eglDisplay, m_eglConfig, pbufAttribs);
    if (m_pbufSurface == EGL_NO_SURFACE) {
      ERROR("Failed to create pbuffer surface: error=0x%x", s_egl.eglGetError());
      return false;
    }
  } else {
    DEBUG("Using a surfaceless EGL context");
    m_pbufSurface = EGL_NO_SURFACE;
  }

  // Make the context current
  ScopedBind bind(this);
  if (!bind.isValid()) {
    ERROR("Failed to make current");
    return false;
  }

  anbox::graphics::GLExtensions gl_extensions{reinterpret_cast<const char *>(s_gles2.glGetString(GL_EXTENSIONS))};
  if (gl_extensions.support("GL_OES_EGL_image")) {
    m_caps.has_eglimage_texture_2d = egl_extensions.support("EGL_KHR_gl_texture_2D_image");
    m_caps.has_eglimage_renderbuffer = egl_extensions.support("EGL_KHR_gl_renderbuffer_image");
  } else {
    m_caps.has_eglimage_texture_2d = false;
    m_caps.has_eglimage_renderbuffer = false;
  }

  // Fail initialization if not all of the following extensions
  // exist:
  //     EGL_KHR_gl_texture_2d_image
  //     GL_OES_EGL_IMAGE
  if (!m_caps.has_eglimage_texture_2d) {
    ERROR("Failed: Missing egl_image related extension(s)");
    bind.release();
    return false;
  }

  // Initialize set of configs
  m_configs = new RendererConfigList(m_eglDisplay);
  if (m_configs->empty()) {
    ERROR("Failed: Initialize set of configs");
    bind.release();
    return false;
  }

  // Check that we have config for each GLES and GLES2
  size_t nConfigs = m_configs->size();
  int nGLConfigs = 0;
  int nGL2Configs = 0;
  for (size_t i = 0; i < nConfigs; ++i) {
    GLint rtype = m_configs->get(i)->getRenderableType();
    if (0 != (rtype & EGL_OPENGL_ES_BIT)) {
      nGLConfigs++;
    }
    if (0 != (rtype & EGL_OPENGL_ES2_BIT)) {
      nGL2Configs++;
    }
  }

  // Fail initialization if no GLES configs exist
  if (nGLConfigs == 0) {
    bind.release();
    return false;
  }

  // If no GLES2 configs exist - not GLES2 capability
  if (nGL2Configs == 0) {
    ERROR("Failed: No GLES 2.x configs found!");
    bind.release();
    return false;
  }

  // Cache the GL strings so we don't have to think about threading or
  // current-context when asked for them.
  m_glVendor = reinterpret_cast<const char *>(s_gles2.glGetString(GL_VENDOR));
  m_glRenderer = reinterpret_cast<const char *>(s_gles2.glGetString(GL_RENDERER));
  m_glVersion = reinterpret_cast<const char *>(s_gles2.glGetString(GL_VERSION));

  const char *egl_vendor = s_egl.eglQueryString(m_eglDisplay, EGL_VENDOR);
  const char *egl_version = s_egl.eglQueryString(m_eglDisplay, EGL_VERSION);
  const char *egl_apis = s_egl.eglQueryString(m_eglDisplay, EGL_CLIENT_APIS);
  std::string identity = std::string(m_glVendor ? m_glVendor : "") + " " +
                         (m_glRenderer ? m_glRenderer : "");
  std::transform(identity.begin(), identity.end(), identity.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  static const char *software_markers[] = {
      "llvmpipe", "softpipe", "swrast", "lavapipe",
      "software rasterizer", "swiftshader"};
  m_softwareRenderer = false;
  for (const auto *marker : software_markers)
    m_softwareRenderer = m_softwareRenderer || identity.find(marker) != std::string::npos;
  INFO("GPU diagnostics: egl_vendor='%s' egl_version='%s' client_apis='%s'",
       egl_vendor ? egl_vendor : "unknown", egl_version ? egl_version : "unknown",
       egl_apis ? egl_apis : "unknown");
  INFO("GPU diagnostics: gl_vendor='%s' gl_renderer='%s' gl_version='%s' acceleration=%s",
       m_glVendor ? m_glVendor : "unknown", m_glRenderer ? m_glRenderer : "unknown",
       m_glVersion ? m_glVersion : "unknown",
       m_softwareRenderer ? "software" : "hardware");

  m_textureDraw = new TextureDraw(m_eglDisplay);
  if (!m_textureDraw || !m_textureDraw->valid()) {
    ERROR("Failed: creation of TextureDraw instance");
    delete m_textureDraw;
    m_textureDraw = nullptr;
    bind.release();
    return false;
  }

  m_defaultProgram = m_family.add_program(vshader, defaultFShader);
  m_alphaProgram = m_family.add_program(vshader, alphaFShader);

  bind.release();

  DEBUG("Successfully initialized EGL");
  m_initialized = true;

  return true;
}

Renderer::Program::Program(GLuint program_id) {
  id = program_id;
  position_attr = s_gles2.glGetAttribLocation(id, "position");
  texcoord_attr = s_gles2.glGetAttribLocation(id, "texcoord");
  tex_uniform = s_gles2.glGetUniformLocation(id, "tex");
  center_uniform = s_gles2.glGetUniformLocation(id, "center");
  display_transform_uniform =
      s_gles2.glGetUniformLocation(id, "display_transform");
  transform_uniform = s_gles2.glGetUniformLocation(id, "transform");
  screen_to_gl_coords_uniform =
      s_gles2.glGetUniformLocation(id, "screen_to_gl_coords");
  alpha_uniform = s_gles2.glGetUniformLocation(id, "alpha");
}

Renderer::Renderer()
    : m_configs(NULL),
      m_eglDisplay(EGL_NO_DISPLAY),
      m_colorBufferBytes(0),
      m_colorBufferHelper(new ColorBufferHelper(this)),
      m_eglContext(EGL_NO_CONTEXT),
      m_pbufSurface(EGL_NO_SURFACE),
      m_pbufContext(EGL_NO_CONTEXT),
      m_prevContext(EGL_NO_CONTEXT),
      m_prevReadSurf(EGL_NO_SURFACE),
      m_prevDrawSurf(EGL_NO_SURFACE),
      m_textureDraw(NULL),
      m_lastPostedColorBuffer(0),
      m_statsNumFrames(0),
      m_statsStartTime(0LL),
      m_glVendor(NULL),
      m_glRenderer(NULL),
      m_glVersion(NULL) {
  m_initialized = false;
  m_softwareRenderer = false;
  m_nativeVisualId = 0;
  m_fpsStats = getenv("SHOW_FPS_STATS") != NULL;
}

Renderer::~Renderer() {
  delete m_textureDraw;
  delete m_configs;
  delete m_colorBufferHelper;
}

RendererWindow *Renderer::createNativeWindow(
    EGLNativeWindowType native_window) {
  std::unique_lock<std::mutex> lock(m_lock);

  auto existing = m_nativeWindows.find(native_window);
  if (existing != m_nativeWindows.end())
    return existing->second;

  auto window = new RendererWindow;
  window->native_window = native_window;
  const auto pending = m_pendingWindowSizes.find(native_window);
  if (pending != m_pendingWindowSizes.end()) {
    window->published_viewport.window_width = pending->second.first;
    window->published_viewport.window_height = pending->second.second;
    m_pendingWindowSizes.erase(pending);
  }
  window->surface = s_egl.eglCreateWindowSurface(
      m_eglDisplay, m_eglConfig, window->native_window, nullptr);
  if (window->surface == EGL_NO_SURFACE) {
    ERROR("Host EGL surface create failed: native=%p error=%#x", native_window,
          s_egl.eglGetError());
    delete window;
    return nullptr;
  }
  invalidateViewportLocked(window, "EGL surface created");

  if (!bindWindow_locked(window)) {
    s_egl.eglDestroySurface(m_eglDisplay, window->surface);
    delete window;
    return nullptr;
  }

  s_gles2.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
                  GL_STENCIL_BUFFER_BIT);
  s_egl.eglSwapBuffers(m_eglDisplay, window->surface);
  s_egl.eglSwapInterval(m_eglDisplay, 1);

  unbind_locked();

  m_nativeWindows.insert({native_window, window});

  return window;
}

void Renderer::updateHostWindow(EGLNativeWindowType native_window,
                                int window_width, int window_height,
                                const char *reason) {
  std::function<void()> redraw;
  {
    std::unique_lock<std::mutex> lock(m_lock);
    const auto entry = m_nativeWindows.find(native_window);
    if (entry == m_nativeWindows.end()) {
      m_pendingWindowSizes[native_window] = {window_width, window_height};
      DEBUG("Viewport lifecycle: pending native=%p event=%s window=%dx%d",
            native_window, reason ? reason : "unknown", window_width,
            window_height);
      return;
    }
    auto &published = entry->second->published_viewport;
    const bool window_geometry_changed =
        published.window_width != window_width ||
        published.window_height != window_height;
    EGLint drawable_width = 0;
    EGLint drawable_height = 0;
    const bool drawable_geometry_known =
        entry->second->surface != EGL_NO_SURFACE &&
        s_egl.eglQuerySurface(m_eglDisplay, entry->second->surface, EGL_WIDTH,
                              &drawable_width) &&
        s_egl.eglQuerySurface(m_eglDisplay, entry->second->surface, EGL_HEIGHT,
                              &drawable_height);
    const bool drawable_geometry_changed = drawable_geometry_known &&
        (published.drawable_width != drawable_width ||
         published.drawable_height != drawable_height);
    published.window_width = window_width;
    published.window_height = window_height;

    // X11 WMs legitimately generate move, expose, map and reparent-related
    // SDL notifications without changing the client drawable.  Invalidating
    // a valid viewport for those events leaves the legacy synchronous
    // composer waiting for an unrelated Android frame before input can be
    // mapped again.  Preserve the published transform when both geometries
    // are unchanged; a real resize/scale transition, a failed query, or an
    // already-invalid viewport still takes the normal redraw path below.
    if (published.valid && !window_geometry_changed &&
        drawable_geometry_known && !drawable_geometry_changed) {
      DEBUG("Viewport lifecycle: native=%p event=%s geometry unchanged at window=%dx%d drawable=%dx%d",
            native_window, reason ? reason : "unknown", window_width,
            window_height, drawable_width, drawable_height);
      return;
    }
    invalidateViewportLocked(entry->second, reason);
    redraw = m_redrawRequester;
  }
  if (redraw)
    redraw();
}

void Renderer::set_redraw_requester(std::function<void()> requester) {
  std::unique_lock<std::mutex> lock(m_lock);
  m_redrawRequester = std::move(requester);
}

bool Renderer::renderedViewport(
    EGLNativeWindowType native_window,
    anbox::graphics::RenderedViewport *viewport) {
  if (!viewport)
    return false;
  std::unique_lock<std::mutex> lock(m_lock);
  const auto entry = m_nativeWindows.find(native_window);
  if (entry == m_nativeWindows.end())
    return false;
  *viewport = entry->second->published_viewport;
  return viewport->valid;
}

void Renderer::destroyNativeWindow(EGLNativeWindowType native_window) {
  std::unique_lock<std::mutex> lock(m_lock);
  m_pendingWindowSizes.erase(native_window);
  auto w = m_nativeWindows.find(native_window);
  if (w == m_nativeWindows.end()) return;

  s_egl.eglMakeCurrent(m_eglDisplay, nullptr, nullptr, nullptr);

  if (w->second->surface != EGL_NO_SURFACE)
    s_egl.eglDestroySurface(m_eglDisplay, w->second->surface);

  delete w->second;
  m_nativeWindows.erase(w);
}

bool Renderer::recreateWindowSurfaceLocked(RendererWindow *window) {
  invalidateViewportLocked(window, "EGL surface recreation");
  s_egl.eglMakeCurrent(m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
  if (window->surface != EGL_NO_SURFACE)
    s_egl.eglDestroySurface(m_eglDisplay, window->surface);
  window->surface = s_egl.eglCreateWindowSurface(
      m_eglDisplay, m_eglConfig, window->native_window, nullptr);
  if (window->surface == EGL_NO_SURFACE) {
    ERROR("Host EGL surface recovery failed: native=%p error=%#x",
          window->native_window, s_egl.eglGetError());
    return false;
  }
  invalidateViewportLocked(window, "EGL surface recreated");
  INFO("Host EGL surface recreated after resize/minimize lifecycle event");
  return true;
}

void Renderer::invalidateViewportLocked(RendererWindow *window,
                                        const char *reason) {
  auto &snapshot = window->published_viewport;
  snapshot.valid = false;
  ++snapshot.generation;
  EGLint drawable_width = 0;
  EGLint drawable_height = 0;
  if (window->surface != EGL_NO_SURFACE) {
    if (!s_egl.eglQuerySurface(m_eglDisplay, window->surface, EGL_WIDTH,
                               &drawable_width) ||
        !s_egl.eglQuerySurface(m_eglDisplay, window->surface, EGL_HEIGHT,
                               &drawable_height)) {
      const auto error = s_egl.eglGetError();
      DEBUG("Viewport lifecycle: EGL drawable query deferred error=%#x",
            error);
      drawable_width = 0;
      drawable_height = 0;
    }
  }
  snapshot.drawable_width = drawable_width;
  snapshot.drawable_height = drawable_height;
  INFO("Viewport lifecycle: native=%p event=%s window=%dx%d drawable=%dx%d generation=%llu valid=0",
       window->native_window, reason ? reason : "unknown",
       snapshot.window_width, snapshot.window_height, drawable_width,
       drawable_height, static_cast<unsigned long long>(snapshot.generation));
}

HandleType Renderer::genHandle() {
  HandleType id;
  do {
    id = ++s_nextHandle;
  } while (id == 0 || m_contexts.find(id) != m_contexts.end() ||
           m_windows.find(id) != m_windows.end());

  return id;
}

HandleType Renderer::createColorBuffer(int p_width, int p_height,
                                       GLenum p_internalFormat) {
  // Android 15's ranchu/SurfaceFlinger path can legitimately allocate more
  // than 512 MiB of 1080p RGBA host color buffers before the display
  // swapchain is established. Keep the allocation/count guards, but allow
  // enough headroom for the modern graphics stack to finish initialization.
  constexpr size_t max_color_buffer_bytes = 2048U * 1024U * 1024U;
  constexpr size_t max_color_buffers = 128U;
  std::unique_lock<std::mutex> l(m_lock);

  HandleType ret = 0;
  if (p_width <= 0 || p_height <= 0) {
    ERROR("Rejecting invalid color buffer allocation %dx%d",
          p_width, p_height);
    return ret;
  }

  const auto width = static_cast<size_t>(p_width);
  const auto height = static_cast<size_t>(p_height);
  if (m_colorbuffers.size() >= max_color_buffers) {
    ERROR("Rejecting color buffer allocation %dx%d format=0x%x current_count=%zu max_count=%zu current_bytes=%zu byte_budget=%zu due to color-buffer count limit",
          p_width, p_height, p_internalFormat, m_colorbuffers.size(),
          max_color_buffers, m_colorBufferBytes, max_color_buffer_bytes);
    return ret;
  }

  if (width > std::numeric_limits<size_t>::max() / height / 8U) {
    ERROR("Rejecting overflowing color buffer allocation %dx%d",
          p_width, p_height);
    return ret;
  }

  // Each ColorBuffer owns two host RGBA textures.
  const size_t allocation_bytes = width * height * 8U;
  if (allocation_bytes > max_color_buffer_bytes ||
      m_colorBufferBytes > max_color_buffer_bytes - allocation_bytes) {
    ERROR("Rejecting color buffer allocation %dx%d format=0x%x bytes=%zu current=%zu buffers=%zu beyond %zu MiB budget",
          p_width, p_height, p_internalFormat, allocation_bytes,
          m_colorBufferBytes, m_colorbuffers.size(),
          max_color_buffer_bytes / (1024U * 1024U));
    return ret;
  }

  ColorBufferPtr cb(ColorBuffer::create(
      getDisplay(), p_width, p_height, p_internalFormat,
      getCaps().has_eglimage_texture_2d, m_colorBufferHelper));
  if (cb) {
    ret = genHandle();
    m_colorbuffers[ret].cb = cb;
    m_colorbuffers[ret].refcount = 1;
    m_colorbuffers[ret].allocation_bytes = allocation_bytes;
    m_colorbuffers[ret].width = p_width;
    m_colorbuffers[ret].height = p_height;
    m_colorbuffers[ret].format = p_internalFormat;
    m_colorBufferBytes += allocation_bytes;
    WARNING("ColorBuffer lifecycle: create handle=%#x size=%dx%d format=0x%x bytes=%zu refcount=%u live_count=%zu live_bytes=%zu",
         ret, p_width, p_height, p_internalFormat, allocation_bytes,
         m_colorbuffers[ret].refcount, m_colorbuffers.size(),
         m_colorBufferBytes);
  } else {
    ERROR("ColorBuffer lifecycle: create failed size=%dx%d format=0x%x live_count=%zu live_bytes=%zu",
          p_width, p_height, p_internalFormat, m_colorbuffers.size(),
          m_colorBufferBytes);
  }
  return ret;
}

HandleType Renderer::createRenderContext(int p_config, HandleType p_share,
                                         unsigned int p_guestGlesVersion) {
  std::unique_lock<std::mutex> l(m_lock);

  HandleType ret = 0;

  const RendererConfig *config = getConfigs()->get(p_config);
  if (!config) {
    return ret;
  }

  RenderContextPtr share(NULL);
  if (p_share != 0) {
    RenderContextMap::iterator s(m_contexts.find(p_share));
    if (s == m_contexts.end()) {
      return ret;
    }
    share = (*s).second;
  }
  EGLContext sharedContext =
      share ? share->getEGLContext() : EGL_NO_CONTEXT;

  RenderContextPtr rctx(RenderContext::create(
      m_eglDisplay, config->getEglConfig(), sharedContext,
      p_guestGlesVersion));
  if (rctx) {
    ret = genHandle();
    m_contexts[ret] = rctx;
    RenderThreadInfo *tinfo = RenderThreadInfo::get();
    tinfo->m_contextSet.insert(ret);
  }
  return ret;
}

HandleType Renderer::createWindowSurface(int p_config, int p_width,
                                         int p_height) {
  std::unique_lock<std::mutex> l(m_lock);

  HandleType ret = 0;

  const RendererConfig *config = getConfigs()->get(p_config);
  if (!config) {
    ERROR("createWindowSurface rejected invalid config=%d size=%dx%d",
          p_config, p_width, p_height);
    return ret;
  }

  WindowSurfacePtr win(WindowSurface::create(
      getDisplay(), config->getEglConfig(), p_width, p_height));
  if (win) {
    ret = genHandle();
    m_windows[ret] = std::pair<WindowSurfacePtr, HandleType>(win, 0);
    RenderThreadInfo *tinfo = RenderThreadInfo::get();
    tinfo->m_windowSet.insert(ret);
    WARNING("ColorBuffer lifecycle: createWindowSurface handle=%#x size=%dx%d tracked_windows=%zu",
         ret, p_width, p_height, m_windows.size());
  } else {
    ERROR("createWindowSurface failed config=%d size=%dx%d egl_error=%#x tracked_windows=%zu",
          p_config, p_width, p_height, s_egl.eglGetError(),
          m_windows.size());
  }

  return ret;
}

void Renderer::drainRenderContext() {
  std::unique_lock<std::mutex> l(m_lock);

  RenderThreadInfo *tinfo = RenderThreadInfo::get();
  if (tinfo->m_contextSet.empty()) return;
  for (std::set<HandleType>::iterator it = tinfo->m_contextSet.begin();
       it != tinfo->m_contextSet.end(); ++it) {
    HandleType contextHandle = *it;
    m_contexts.erase(contextHandle);
  }
  tinfo->m_contextSet.clear();
}

bool Renderer::releaseWindowSurfaceColorBufferLocked(
    HandleType windowHandle, HandleType colorBufferHandle,
    const char* source) {
  if (!colorBufferHandle)
    return true;

  ColorBufferMap::iterator cit(m_colorbuffers.find(colorBufferHandle));
  if (cit == m_colorbuffers.end()) {
    WARNING("ColorBuffer lifecycle: %s window=%#x attached missing handle=%#x live_count=%zu live_bytes=%zu",
         source, windowHandle, colorBufferHandle, m_colorbuffers.size(),
         m_colorBufferBytes);
    return false;
  }

  ColorBufferRef &ref = (*cit).second;
  if (ref.refcount == 0) {
    ERROR("ColorBuffer lifecycle: %s window=%#x handle=%#x refcount underflow prevented live_count=%zu live_bytes=%zu",
         source, windowHandle, colorBufferHandle, m_colorbuffers.size(),
         m_colorBufferBytes);
    return false;
  }

  WARNING("ColorBuffer lifecycle: %s window=%#x releasing attached handle=%#x size=%dx%d format=0x%x bytes=%zu refcount_before=%u live_count=%zu live_bytes=%zu",
       source, windowHandle, colorBufferHandle, ref.width, ref.height,
       ref.format, ref.allocation_bytes, ref.refcount,
       m_colorbuffers.size(), m_colorBufferBytes);
  if (--ref.refcount == 0) {
    m_colorBufferBytes -= ref.allocation_bytes;
    m_colorbuffers.erase(cit);
    WARNING("ColorBuffer lifecycle: free source=%s handle=%#x live_count=%zu live_bytes=%zu",
         source, colorBufferHandle, m_colorbuffers.size(),
         m_colorBufferBytes);
  } else {
    WARNING("ColorBuffer lifecycle: %s kept handle=%#x refcount_after=%u live_count=%zu live_bytes=%zu",
         source, colorBufferHandle, ref.refcount, m_colorbuffers.size(),
         m_colorBufferBytes);
  }

  return true;
}

void Renderer::drainWindowSurface() {
  std::unique_lock<std::mutex> l(m_lock);

  RenderThreadInfo *tinfo = RenderThreadInfo::get();
  if (tinfo->m_windowSet.empty()) return;
  for (std::set<HandleType>::iterator it = tinfo->m_windowSet.begin();
       it != tinfo->m_windowSet.end(); ++it) {
    HandleType windowHandle = *it;
    if (m_windows.find(windowHandle) != m_windows.end()) {
      HandleType oldColorBufferHandle = m_windows[windowHandle].second;
      releaseWindowSurfaceColorBufferLocked(
          windowHandle, oldColorBufferHandle, "drainWindowSurface");
      WARNING("ColorBuffer lifecycle: drainWindowSurface erase window=%#x attached_handle=%#x tracked_windows_before=%zu",
           windowHandle, oldColorBufferHandle, m_windows.size());
      m_windows.erase(windowHandle);
    }
  }
  tinfo->m_windowSet.clear();
}

void Renderer::destroyDeferredWindowLocked(HandleType windowHandle,
                                           const char* source) {
  WindowSurfaceMap::iterator window = m_windows.find(windowHandle);
  if (window == m_windows.end())
    return;
  releaseWindowSurfaceColorBufferLocked(windowHandle, window->second.second,
                                        source);
  m_windows.erase(window);
}

void Renderer::reapDeferredResourcesLocked() {
  const DeferredClock::time_point now = DeferredClock::now();
  for (DeferredResourceMap::iterator it = m_deferredWindows.begin();
       it != m_deferredWindows.end();) {
    if (it->second > now) {
      ++it;
      continue;
    }
    const HandleType handle = it->first;
    it = m_deferredWindows.erase(it);
    WARNING("GL deferred cleanup: reaping window=%#x pending_windows=%zu",
            handle, m_deferredWindows.size());
    destroyDeferredWindowLocked(handle, "deferred GL pipe EOF");
  }
  for (DeferredResourceMap::iterator it = m_deferredContexts.begin();
       it != m_deferredContexts.end();) {
    if (it->second > now) {
      ++it;
      continue;
    }
    const HandleType handle = it->first;
    it = m_deferredContexts.erase(it);
    WARNING("GL deferred cleanup: reaping context=%#x pending_contexts=%zu",
            handle, m_deferredContexts.size());
    m_contexts.erase(handle);
  }
}

void Renderer::cancelDeferredContextLocked(HandleType context) {
  if (m_deferredContexts.erase(context) != 0)
    WARNING("GL deferred cleanup: preserved context=%#x on valid reuse", context);
}

void Renderer::cancelDeferredWindowLocked(HandleType window) {
  if (m_deferredWindows.erase(window) != 0)
    WARNING("GL deferred cleanup: preserved window=%#x on valid reuse", window);
}

void Renderer::deferCurrentThreadResources() {
  constexpr size_t kMaxDeferredContexts = 64;
  constexpr size_t kMaxDeferredWindows = 64;
  constexpr std::chrono::seconds kDeferredLifetime(2);

  std::unique_lock<std::mutex> l(m_lock);
  reapDeferredResourcesLocked();
  RenderThreadInfo *tinfo = RenderThreadInfo::get();
  const DeferredClock::time_point expiry = DeferredClock::now() + kDeferredLifetime;
  for (ThreadContextSet::const_iterator it = tinfo->m_contextSet.begin();
       it != tinfo->m_contextSet.end(); ++it) {
    if (m_contexts.find(*it) == m_contexts.end())
      continue;
    if (m_deferredContexts.size() >= kMaxDeferredContexts &&
        m_deferredContexts.find(*it) == m_deferredContexts.end()) {
      WARNING("GL deferred cleanup: context capacity reached; destroying context=%#x immediately", *it);
      m_contexts.erase(*it);
      continue;
    }
    m_deferredContexts[*it] = expiry;
    WARNING("GL deferred cleanup: deferred context=%#x pending_contexts=%zu",
            *it, m_deferredContexts.size());
  }
  for (WindowSurfaceSet::const_iterator it = tinfo->m_windowSet.begin();
       it != tinfo->m_windowSet.end(); ++it) {
    if (m_windows.find(*it) == m_windows.end())
      continue;
    if (m_deferredWindows.size() >= kMaxDeferredWindows &&
        m_deferredWindows.find(*it) == m_deferredWindows.end()) {
      WARNING("GL deferred cleanup: window capacity reached; destroying window=%#x immediately", *it);
      destroyDeferredWindowLocked(*it, "deferred cleanup capacity");
      continue;
    }
    m_deferredWindows[*it] = expiry;
    WARNING("GL deferred cleanup: deferred window=%#x pending_windows=%zu",
            *it, m_deferredWindows.size());
  }
  tinfo->m_contextSet.clear();
  tinfo->m_windowSet.clear();
}

void Renderer::DestroyRenderContext(HandleType p_context) {
  std::unique_lock<std::mutex> l(m_lock);

  reapDeferredResourcesLocked();
  m_deferredContexts.erase(p_context);
  m_contexts.erase(p_context);
  RenderThreadInfo *tinfo = RenderThreadInfo::get();
  if (tinfo->m_contextSet.empty()) return;
  tinfo->m_contextSet.erase(p_context);
}

void Renderer::DestroyWindowSurface(HandleType p_surface) {
  std::unique_lock<std::mutex> l(m_lock);

  reapDeferredResourcesLocked();
  m_deferredWindows.erase(p_surface);
  auto w = m_windows.find(p_surface);
  if (w != m_windows.end()) {
    HandleType oldColorBufferHandle = (*w).second.second;
    if (oldColorBufferHandle) {
      auto c = m_colorbuffers.find(oldColorBufferHandle);
      if (c != m_colorbuffers.end()) {
        const ColorBufferRef &ref = (*c).second;
        WARNING("ColorBuffer lifecycle: DestroyWindowSurface window=%#x erasing with attached handle=%#x size=%dx%d format=0x%x bytes=%zu refcount=%u live_count=%zu live_bytes=%zu",
             p_surface, oldColorBufferHandle, ref.width, ref.height,
             ref.format, ref.allocation_bytes, ref.refcount,
             m_colorbuffers.size(), m_colorBufferBytes);
      } else {
        WARNING("ColorBuffer lifecycle: DestroyWindowSurface window=%#x attached missing handle=%#x live_count=%zu live_bytes=%zu",
             p_surface, oldColorBufferHandle, m_colorbuffers.size(),
             m_colorBufferBytes);
      }
    } else {
      WARNING("ColorBuffer lifecycle: DestroyWindowSurface window=%#x no attached color buffer live_count=%zu live_bytes=%zu",
           p_surface, m_colorbuffers.size(), m_colorBufferBytes);
    }
    releaseWindowSurfaceColorBufferLocked(
        p_surface, oldColorBufferHandle, "DestroyWindowSurface");
    m_windows.erase(w);
    RenderThreadInfo *tinfo = RenderThreadInfo::get();
    if (tinfo->m_windowSet.empty()) return;
    tinfo->m_windowSet.erase(p_surface);
  }
}

int Renderer::openColorBuffer(HandleType p_colorbuffer) {
  std::unique_lock<std::mutex> l(m_lock);

  ColorBufferMap::iterator c(m_colorbuffers.find(p_colorbuffer));
  if (c == m_colorbuffers.end()) {
    // bad colorbuffer handle
    ERROR("FB: openColorBuffer cb handle %#x not found", p_colorbuffer);
    return -1;
  }
  if ((*c).second.refcount == std::numeric_limits<uint32_t>::max()) {
    ERROR("FB: color buffer %#x reference count overflow", p_colorbuffer);
    return -1;
  }
  WARNING("ColorBuffer lifecycle: open handle=%#x size=%dx%d format=0x%x bytes=%zu refcount_before=%u live_count=%zu live_bytes=%zu",
       p_colorbuffer, (*c).second.width, (*c).second.height,
       (*c).second.format, (*c).second.allocation_bytes,
       (*c).second.refcount, m_colorbuffers.size(), m_colorBufferBytes);
  (*c).second.refcount++;
  WARNING("ColorBuffer lifecycle: open handle=%#x refcount_after=%u live_count=%zu live_bytes=%zu",
       p_colorbuffer, (*c).second.refcount, m_colorbuffers.size(),
       m_colorBufferBytes);
  return 0;
}

void Renderer::closeColorBuffer(HandleType p_colorbuffer) {
  std::unique_lock<std::mutex> l(m_lock);

  ColorBufferMap::iterator c(m_colorbuffers.find(p_colorbuffer));
  if (c == m_colorbuffers.end()) {
    // This is harmless: it is normal for guest system to issue
    // closeColorBuffer command when the color buffer is already
    // garbage collected on the host. (we dont have a mechanism
    // to give guest a notice yet)
    WARNING("ColorBuffer lifecycle: close missing handle=%#x live_count=%zu live_bytes=%zu",
         p_colorbuffer, m_colorbuffers.size(), m_colorBufferBytes);
    return;
  }
  if ((*c).second.refcount == 0) {
    ERROR("FB: color buffer %#x reference count underflow", p_colorbuffer);
    return;
  }
  WARNING("ColorBuffer lifecycle: close handle=%#x size=%dx%d format=0x%x bytes=%zu refcount_before=%u live_count=%zu live_bytes=%zu",
       p_colorbuffer, (*c).second.width, (*c).second.height,
       (*c).second.format, (*c).second.allocation_bytes,
       (*c).second.refcount, m_colorbuffers.size(), m_colorBufferBytes);
  if (--(*c).second.refcount == 0) {
    m_colorBufferBytes -= (*c).second.allocation_bytes;
    m_colorbuffers.erase(c);
    WARNING("ColorBuffer lifecycle: free source=closeColorBuffer handle=%#x live_count=%zu live_bytes=%zu",
         p_colorbuffer, m_colorbuffers.size(), m_colorBufferBytes);
  } else {
    WARNING("ColorBuffer lifecycle: close kept handle=%#x refcount_after=%u live_count=%zu live_bytes=%zu",
         p_colorbuffer, (*c).second.refcount, m_colorbuffers.size(),
         m_colorBufferBytes);
  }
}

bool Renderer::flushWindowSurfaceColorBuffer(HandleType p_surface) {
  std::unique_lock<std::mutex> l(m_lock);

  WindowSurfaceMap::iterator w(m_windows.find(p_surface));
  if (w == m_windows.end()) {
    ERROR("FB::flushWindowSurfaceColorBuffer: window handle %#x not found",
        p_surface);
    // bad surface handle
    return false;
  }

  auto surface = (*w).second.first;
  if (!surface)
    return false;

  surface->flushColorBuffer();

  return true;
}

bool Renderer::retainPostedColorBuffer(HandleType p_colorbuffer, int* width,
                                       int* height) {
  std::unique_lock<std::mutex> l(m_lock);
  reapDeferredResourcesLocked();
  ColorBufferMap::iterator next = m_colorbuffers.find(p_colorbuffer);
  if (next == m_colorbuffers.end()) {
    ERROR("FB::postColorBuffer: bad color buffer handle %#x", p_colorbuffer);
    return false;
  }
  if (m_lastPostedColorBuffer != p_colorbuffer) {
    if (next->second.refcount == std::numeric_limits<uint32_t>::max()) {
      ERROR("FB::postColorBuffer: color buffer %#x reference count overflow",
            p_colorbuffer);
      return false;
    }
    const HandleType previous = m_lastPostedColorBuffer;
    ++next->second.refcount;
    m_lastPostedColorBuffer = p_colorbuffer;
    if (previous)
      releaseWindowSurfaceColorBufferLocked(0, previous, "rcFBPost replacement");
  }
  *width = next->second.width;
  *height = next->second.height;
  WARNING("Presentation trace: retained posted color_buffer=%#x size=%dx%d refcount=%u",
          p_colorbuffer, *width, *height, next->second.refcount);
  return true;
}

bool Renderer::setWindowSurfaceColorBuffer(HandleType p_surface,
                                           HandleType p_colorbuffer) {
  std::unique_lock<std::mutex> l(m_lock);
  reapDeferredResourcesLocked();
  cancelDeferredWindowLocked(p_surface);

  WindowSurfaceMap::iterator w(m_windows.find(p_surface));
  if (w == m_windows.end()) {
    // bad surface handle
    ERROR("%s: bad window surface handle %#x", __FUNCTION__, p_surface);
    return false;
  }

  ColorBufferPtr newColorBuffer;
  ColorBufferRef* newRef = NULL;
  if (p_colorbuffer) {
    ColorBufferMap::iterator c(m_colorbuffers.find(p_colorbuffer));
    if (c == m_colorbuffers.end()) {
      DEBUG("%s: bad color buffer handle %#x", __FUNCTION__, p_colorbuffer);
      WARNING("ColorBuffer lifecycle: setWindowSurfaceColorBuffer window=%#x old_handle=%#x new_handle=%#x new missing; keeping old attachment live_count=%zu live_bytes=%zu",
           p_surface, (*w).second.second, p_colorbuffer,
           m_colorbuffers.size(), m_colorBufferBytes);
      // bad colorbuffer handle
      return false;
    }
    newColorBuffer = (*c).second.cb;
    newRef = &(*c).second;
  }

  HandleType oldColorBufferHandle = (*w).second.second;
  if (oldColorBufferHandle == p_colorbuffer) {
    if (newColorBuffer)
      (*w).second.first->setColorBuffer(newColorBuffer);
    WARNING("ColorBuffer lifecycle: setWindowSurfaceColorBuffer window=%#x old_handle=%#x new_handle=%#x unchanged live_count=%zu live_bytes=%zu",
         p_surface, oldColorBufferHandle, p_colorbuffer,
         m_colorbuffers.size(), m_colorBufferBytes);
    return true;
  }

  if (newRef && newRef->refcount == std::numeric_limits<uint32_t>::max()) {
    ERROR("FB: color buffer %#x window reference count overflow",
          p_colorbuffer);
    return false;
  }

  releaseWindowSurfaceColorBufferLocked(
      p_surface, oldColorBufferHandle, "setWindowSurfaceColorBuffer");
  if (newRef)
    ++newRef->refcount;
  (*w).second.first->setColorBuffer(newColorBuffer);
  WARNING("ColorBuffer lifecycle: setWindowSurfaceColorBuffer window=%#x old_handle=%#x new_handle=%#x size=%dx%d format=0x%x bytes=%zu refcount=%u live_count=%zu live_bytes=%zu",
       p_surface, oldColorBufferHandle, p_colorbuffer,
       newRef ? newRef->width : 0, newRef ? newRef->height : 0,
       newRef ? newRef->format : 0, newRef ? newRef->allocation_bytes : 0,
       newRef ? newRef->refcount : 0, m_colorbuffers.size(),
       m_colorBufferBytes);
  (*w).second.second = p_colorbuffer;
  return true;
}

void Renderer::readColorBuffer(HandleType p_colorbuffer, int x, int y,
                               int width, int height, GLenum format,
                               GLenum type, void *pixels) {
  std::unique_lock<std::mutex> l(m_lock);

  ColorBufferMap::iterator c(m_colorbuffers.find(p_colorbuffer));
  if (c == m_colorbuffers.end()) {
    // bad colorbuffer handle
    return;
  }

  (*c).second.cb->readPixels(x, y, width, height, format, type, pixels);
}

bool Renderer::updateColorBuffer(HandleType p_colorbuffer, int x, int y,
                                 int width, int height, GLenum format,
                                 GLenum type, void *pixels) {
  std::unique_lock<std::mutex> l(m_lock);

  ColorBufferMap::iterator c(m_colorbuffers.find(p_colorbuffer));
  if (c == m_colorbuffers.end()) {
    // bad colorbuffer handle
    return false;
  }

  (*c).second.cb->subUpdate(x, y, width, height, format, type, pixels);

  return true;
}

bool Renderer::bindColorBufferToTexture(HandleType p_colorbuffer) {
  std::unique_lock<std::mutex> l(m_lock);

  ColorBufferMap::iterator c(m_colorbuffers.find(p_colorbuffer));
  if (c == m_colorbuffers.end()) {
    // bad colorbuffer handle
    return false;
  }

  return (*c).second.cb->bindToTexture();
}

bool Renderer::bindColorBufferToRenderbuffer(HandleType p_colorbuffer) {
  std::unique_lock<std::mutex> l(m_lock);

  ColorBufferMap::iterator c(m_colorbuffers.find(p_colorbuffer));
  if (c == m_colorbuffers.end()) {
    // bad colorbuffer handle
    return false;
  }

  return (*c).second.cb->bindToRenderbuffer();
}

bool Renderer::bindContext(HandleType p_context, HandleType p_drawSurface,
                           HandleType p_readSurface) {
  std::unique_lock<std::mutex> l(m_lock);
  reapDeferredResourcesLocked();

  WindowSurfacePtr draw(NULL), read(NULL);
  RenderContextPtr ctx(NULL);

  //
  // if this is not an unbind operation - make sure all handles are good
  //
  if (p_context || p_drawSurface || p_readSurface) {
    RenderContextMap::iterator r(m_contexts.find(p_context));
    if (r == m_contexts.end()) {
      // bad context handle
      return false;
    }
    cancelDeferredContextLocked(p_context);

    ctx = (*r).second;
    WindowSurfaceMap::iterator w(m_windows.find(p_drawSurface));
    if (w == m_windows.end()) {
      // bad surface handle
      return false;
    }
    cancelDeferredWindowLocked(p_drawSurface);
    draw = (*w).second.first;

    if (p_readSurface != p_drawSurface) {
      WindowSurfaceMap::iterator w(m_windows.find(p_readSurface));
      if (w == m_windows.end()) {
        // bad surface handle
        return false;
      }
      cancelDeferredWindowLocked(p_readSurface);
      read = (*w).second.first;
    } else {
      read = draw;
    }
  }

  if (!s_egl.eglMakeCurrent(m_eglDisplay,
                            draw ? draw->getEGLSurface() : EGL_NO_SURFACE,
                            read ? read->getEGLSurface() : EGL_NO_SURFACE,
                            ctx ? ctx->getEGLContext() : EGL_NO_CONTEXT)) {
    ERROR("eglMakeCurrent failed: 0x%04x", s_egl.eglGetError());
    return false;
  }

  //
  // Bind the surface(s) to the context
  //
  RenderThreadInfo *tinfo = RenderThreadInfo::get();
  WindowSurfacePtr bindDraw, bindRead;
  if (!draw && !read) {
    // Unbind the current read and draw surfaces from the context
    bindDraw = tinfo->currDrawSurf;
    bindRead = tinfo->currReadSurf;
  } else {
    bindDraw = draw;
    bindRead = read;
  }

  if (bindDraw && bindRead) {
    if (bindDraw != bindRead) {
      bindDraw->bind(ctx, WindowSurface::BIND_DRAW);
      bindRead->bind(ctx, WindowSurface::BIND_READ);
    } else {
      bindDraw->bind(ctx, WindowSurface::BIND_READDRAW);
    }
  }

  //
  // update thread info with current bound context
  //
  tinfo->currContext = ctx;
  tinfo->currDrawSurf = draw;
  tinfo->currReadSurf = read;
  if (ctx) {
    if (ctx->isGL2())
      tinfo->m_gl2Dec.setContextData(&ctx->decoderContextData());
    else
      tinfo->m_glDec.setContextData(&ctx->decoderContextData());
  } else {
    tinfo->m_glDec.setContextData(NULL);
    tinfo->m_gl2Dec.setContextData(NULL);
  }
  return true;
}

HandleType Renderer::createClientImage(HandleType context, EGLenum target,
                                       GLuint buffer) {
  RenderContextPtr ctx(NULL);

  if (context) {
    RenderContextMap::iterator r(m_contexts.find(context));
    if (r == m_contexts.end()) {
      // bad context handle
      return false;
    }

    ctx = (*r).second;
  }

  EGLContext eglContext = ctx ? ctx->getEGLContext() : EGL_NO_CONTEXT;
  EGLImageKHR image =
      s_egl.eglCreateImageKHR(m_eglDisplay, eglContext, target,
                              reinterpret_cast<EGLClientBuffer>(buffer), NULL);

  return static_cast<HandleType>(reinterpret_cast<uintptr_t>(image));
}

EGLBoolean Renderer::destroyClientImage(HandleType image) {
  return s_egl.eglDestroyImageKHR(m_eglDisplay,
                                  reinterpret_cast<EGLImageKHR>(image));
}

//
// The framebuffer lock should be held when calling this function !
//
bool Renderer::bind_locked() {
  EGLContext prevContext = s_egl.eglGetCurrentContext();
  EGLSurface prevReadSurf = s_egl.eglGetCurrentSurface(EGL_READ);
  EGLSurface prevDrawSurf = s_egl.eglGetCurrentSurface(EGL_DRAW);

  if (!s_egl.eglMakeCurrent(m_eglDisplay, m_pbufSurface, m_pbufSurface,
                            m_pbufContext)) {
    ERROR("eglMakeCurrent failed: 0x%04x", s_egl.eglGetError());
    return false;
  }

  m_prevContext = prevContext;
  m_prevReadSurf = prevReadSurf;
  m_prevDrawSurf = prevDrawSurf;
  return true;
}

bool Renderer::bindWindow_locked(RendererWindow *window) {
  EGLContext prevContext = s_egl.eglGetCurrentContext();
  EGLSurface prevReadSurf = s_egl.eglGetCurrentSurface(EGL_READ);
  EGLSurface prevDrawSurf = s_egl.eglGetCurrentSurface(EGL_DRAW);

  if (!s_egl.eglMakeCurrent(m_eglDisplay, window->surface, window->surface,
                            m_eglContext)) {
    ERROR("Host eglMakeCurrent failed: error=%#x", s_egl.eglGetError());
    return false;
  }

  m_prevContext = prevContext;
  m_prevReadSurf = prevReadSurf;
  m_prevDrawSurf = prevDrawSurf;
  return true;
}

bool Renderer::unbind_locked() {
  if (!s_egl.eglMakeCurrent(m_eglDisplay, m_prevDrawSurf, m_prevReadSurf,
                            m_prevContext)) {
    return false;
  }

  m_prevContext = EGL_NO_CONTEXT;
  m_prevReadSurf = EGL_NO_SURFACE;
  m_prevDrawSurf = EGL_NO_SURFACE;
  return true;
}

const GLchar *const Renderer::vshader = {
    "attribute vec3 position;"
    "attribute vec2 texcoord;"
    "uniform mat4 screen_to_gl_coords;"
    "uniform mat4 display_transform;"
    "uniform mat4 transform;"
    "uniform vec2 center;"
    "varying vec2 v_texcoord;"
    "void main() {"
    "   vec4 mid = vec4(center, 0.0, 0.0);"
    "   vec4 transformed = (transform * (vec4(position, 1.0) - mid)) + mid;"
    "   gl_Position = display_transform * screen_to_gl_coords * transformed;"
    "   v_texcoord = texcoord;"
    "}"};

const GLchar *const Renderer::alphaFShader = {
    "precision mediump float;"
    "uniform sampler2D tex;"
    "uniform float alpha;"
    "varying vec2 v_texcoord;"
    "void main() {"
    "   vec4 frag = texture2D(tex, v_texcoord);"
    "   gl_FragColor = alpha*frag;"
    "}"};

const GLchar *const Renderer::defaultFShader =
    {  // This is the fastest fragment shader. Use it when you can.
        "precision mediump float;"
        "uniform sampler2D tex;"
        "varying vec2 v_texcoord;"
        "void main() {"
        "   gl_FragColor = texture2D(tex, v_texcoord);"
        "}"};

void Renderer::setupViewport(RendererWindow *window,
                             const anbox::graphics::Rect &rect) {
  /*
   * Here we provide a 3D perspective projection with a default 30 degrees
   * vertical field of view. This projection matrix is carefully designed
   * such that any vertices at depth z=0 will fit the screen coordinates. So
   * client texels will fit screen pixels perfectly as long as the surface is
   * at depth zero. But if you want to do anything fancy, you can also choose
   * a different depth and it will appear to come out of or go into the
   * screen.
   */
  window->screen_to_gl_coords =
      glm::translate(glm::mat4(1.0f), glm::vec3{-1.0f, 1.0f, 0.0f});

  /*
   * Perspective division is one thing that can't be done in a matrix
   * multiplication. It happens after the matrix multiplications. GL just
   * scales {x,y} by 1/w. So modify the final part of the projection matrix
   * to set w ([3]) to be the incoming z coordinate ([2]).
   */
  window->screen_to_gl_coords[2][3] = -1.0f;

  float const vertical_fov_degrees = 30.0f;
  float const near = (rect.height() / 2.0f) /
                     std::tan((vertical_fov_degrees * M_PI / 180.0f) / 2.0f);
  float const far = -near;

  window->screen_to_gl_coords =
      glm::scale(window->screen_to_gl_coords,
                 glm::vec3{2.0f / rect.width(), -2.0f / rect.height(),
                           2.0f / (near - far)});
  window->screen_to_gl_coords = glm::translate(
      window->screen_to_gl_coords, glm::vec3{-rect.left(), -rect.top(), 0.0f});

  window->viewport = rect;
}

void Renderer::tessellate(std::vector<anbox::graphics::Primitive> &primitives,
                          const anbox::graphics::Rect &buf_size,
                          const Renderable &renderable) {
  auto rect = renderable.screen_position();
  GLfloat left = rect.left();
  GLfloat right = rect.right();
  GLfloat top = rect.top();
  GLfloat bottom = rect.bottom();

  anbox::graphics::Primitive rectangle;
  rectangle.tex_id = 0;
  rectangle.type = GL_TRIANGLE_STRIP;

  GLfloat tex_left =
      static_cast<GLfloat>(renderable.crop().left()) / buf_size.width();
  GLfloat tex_top =
      static_cast<GLfloat>(renderable.crop().top()) / buf_size.height();
  GLfloat tex_right =
      static_cast<GLfloat>(renderable.crop().right()) / buf_size.width();
  GLfloat tex_bottom =
      static_cast<GLfloat>(renderable.crop().bottom()) / buf_size.height();

  auto &vertices = rectangle.vertices;
  vertices[0] = {{left, top, 0.0f}, {tex_left, tex_top}};
  vertices[1] = {{left, bottom, 0.0f}, {tex_left, tex_bottom}};
  vertices[2] = {{right, top, 0.0f}, {tex_right, tex_top}};
  vertices[3] = {{right, bottom, 0.0f}, {tex_right, tex_bottom}};

  primitives.resize(1);
  primitives[0] = rectangle;
}

void Renderer::draw(RendererWindow *window, const Renderable &renderable,
                    const Program &prog) {
  const auto &color_buffer = m_colorbuffers.find(renderable.buffer());
  if (color_buffer == m_colorbuffers.end()) return;

  const auto &cb = color_buffer->second.cb;

  s_gles2.glUseProgram(prog.id);
  s_gles2.glUniform1i(prog.tex_uniform, 0);
  s_gles2.glUniformMatrix4fv(prog.display_transform_uniform, 1, GL_FALSE,
                             glm::value_ptr(window->display_transform));
  s_gles2.glUniformMatrix4fv(prog.screen_to_gl_coords_uniform, 1, GL_FALSE,
                             glm::value_ptr(window->screen_to_gl_coords));

  s_gles2.glActiveTexture(GL_TEXTURE0);

  auto const &rect = renderable.screen_position();
  GLfloat centerx = rect.left() + rect.width() / 2.0f;
  GLfloat centery = rect.top() + rect.height() / 2.0f;
  s_gles2.glUniform2f(prog.center_uniform, centerx, centery);

  s_gles2.glUniformMatrix4fv(prog.transform_uniform, 1, GL_FALSE,
                             glm::value_ptr(renderable.transformation()));

  if (prog.alpha_uniform >= 0)
    s_gles2.glUniform1f(prog.alpha_uniform, renderable.alpha());

  s_gles2.glEnableVertexAttribArray(prog.position_attr);
  s_gles2.glEnableVertexAttribArray(prog.texcoord_attr);

  m_primitives.clear();
  tessellate(m_primitives, {
             static_cast<int32_t>(cb->getWidth()),
             static_cast<int32_t>(cb->getHeight())}, renderable);

  for (auto const &p : m_primitives) {
    cb->bind();

    // Color-buffer preparation may perform offscreen GL work. Reassert the
    // presentation program at the ownership boundary even though helpers are
    // required to restore state themselves.
    s_gles2.glUseProgram(prog.id);

    s_gles2.glVertexAttribPointer(prog.position_attr, 3, GL_FLOAT, GL_FALSE,
                                  sizeof(anbox::graphics::Vertex),
                                  &p.vertices[0].position);
    s_gles2.glVertexAttribPointer(prog.texcoord_attr, 2, GL_FLOAT, GL_FALSE,
                                  sizeof(anbox::graphics::Vertex),
                                  &p.vertices[0].texcoord);

    s_gles2.glEnable(GL_BLEND);
    s_gles2.glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                                GL_ONE_MINUS_SRC_ALPHA);

    s_gles2.glDrawArrays(p.type, 0, p.nvertices);
  }

  s_gles2.glDisableVertexAttribArray(prog.texcoord_attr);
  s_gles2.glDisableVertexAttribArray(prog.position_attr);
}

bool Renderer::draw(EGLNativeWindowType native_window,
                    const anbox::graphics::Rect &window_frame,
                    const RenderableList &renderables) {
  (void)window_frame;
  // Decoder threads already take this lock before m_lock.  Keep the same
  // order here: rendering the host window must not overlap guest GLES calls
  // in the shared Mesa/EGL driver.
  std::unique_lock<std::recursive_mutex> host_gles_lock(
      anbox::graphics::OpenGlesMessageProcessor::host_gles_lock());
  std::unique_lock<std::mutex> l(m_lock);

  auto w = m_nativeWindows.find(native_window);
  if (w == m_nativeWindows.end()) {
    ERROR("Presentation trace: host draw missing native window");
    return false;
  }

  if (!bindWindow_locked(w->second)) {
    if (!recreateWindowSurfaceLocked(w->second) ||
        !bindWindow_locked(w->second))
      return false;
  }

  EGLint surface_width = 0;
  EGLint surface_height = 0;
  const bool queried_width = s_egl.eglQuerySurface(
      m_eglDisplay, w->second->surface, EGL_WIDTH, &surface_width);
  const bool queried_height = s_egl.eglQuerySurface(
      m_eglDisplay, w->second->surface, EGL_HEIGHT, &surface_height);
  if (!queried_width || !queried_height) {
    ERROR("Viewport lifecycle: failed to query EGL drawable native=%p error=%#x",
          native_window, s_egl.eglGetError());
    invalidateViewportLocked(w->second, "EGL drawable query failure");
    unbind_locked();
    return false;
  }
  if (surface_width <= 0 || surface_height <= 0) {
    invalidateViewportLocked(w->second, "zero-sized EGL drawable");
    unbind_locked();
    return true;
  }

  int source_width = m_externalFrameSource
      ? (m_externalFrameWidth > 0 ? m_externalFrameWidth
                                 : m_externalExpectedWidth)
      : 0;
  int source_height = m_externalFrameSource
      ? (m_externalFrameHeight > 0 ? m_externalFrameHeight
                                  : m_externalExpectedHeight)
      : 0;
  if (!m_externalFrameSource) {
    for (const auto &renderable : renderables) {
      source_width = std::max(source_width,
                              renderable.screen_position().right());
      source_height = std::max(source_height,
                               renderable.screen_position().bottom());
    }
  }
  if (source_width <= 0 || source_height <= 0) {
    source_width = surface_width;
    source_height = surface_height;
  }
  const anbox::graphics::CoordinateTransform coordinate_transform(
      surface_width, surface_height, source_width, source_height);
  const auto content = coordinate_transform.viewport();

  static unsigned int presentation_sample_counter = 0;
  const bool sample_presentation =
      (++presentation_sample_counter % 60) == 1;
  if (sample_presentation)
    WARNING("Presentation trace: host draw native=%p drawable=%dx%d rendered=%d,%d %dx%d layers=%zu",
            native_window, surface_width, surface_height, content.left(),
            content.top(), content.width(), content.height(), renderables.size());

  setupViewport(w->second, {0, 0, source_width, source_height});
  s_gles2.glViewport(content.left(), content.top(), content.width(),
                     content.height());
  // The host-window pass owns the complete drawable. Guest and offscreen
  // operations may leave a smaller scissor rectangle current.
  s_gles2.glDisable(GL_SCISSOR_TEST);

  // glViewport is now authoritative. Publish the exact values used above;
  // input remains suppressed between lifecycle invalidation and this point.
  auto &published = w->second->published_viewport;
  if (published.window_width <= 0 || published.window_height <= 0) {
    published.window_width = surface_width;
    published.window_height = surface_height;
  }
  const bool viewport_changed = !published.valid ||
      published.drawable_width != surface_width ||
      published.drawable_height != surface_height ||
      published.viewport != content || published.android_width != source_width ||
      published.android_height != source_height ||
      published.rotation != anbox::graphics::CoordinateTransform::Rotation::R0;
  if (viewport_changed)
    ++published.generation;
  published.drawable_width = surface_width;
  published.drawable_height = surface_height;
  published.viewport = content;
  published.android_width = source_width;
  published.android_height = source_height;
  published.rotation = anbox::graphics::CoordinateTransform::Rotation::R0;
  published.valid = true;
  if (viewport_changed) {
    INFO("Viewport published: native=%p window=%dx%d drawable=%dx%d viewport=%d,%d %dx%d android=%dx%d rotation=0 generation=%llu",
         native_window, published.window_width, published.window_height,
         surface_width, surface_height, content.left(), content.top(),
         content.width(), content.height(), source_width, source_height,
         static_cast<unsigned long long>(published.generation));
  }
  s_gles2.glClearColor(0.0, 0.0, 0.0, 1.0);
  s_gles2.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  s_gles2.glClear(GL_COLOR_BUFFER_BIT);

  if (m_externalFrameSource && !m_externalFramePixels.empty()) {
    if (!m_externalFrameTexture) {
      s_gles2.glGenTextures(1, &m_externalFrameTexture);
      s_gles2.glBindTexture(GL_TEXTURE_2D, m_externalFrameTexture);
      s_gles2.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      s_gles2.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      s_gles2.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                              GL_CLAMP_TO_EDGE);
      s_gles2.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                              GL_CLAMP_TO_EDGE);
    }
    if (m_externalUploadedGeneration != m_externalFrameGeneration) {
      s_gles2.glBindTexture(GL_TEXTURE_2D, m_externalFrameTexture);
      s_gles2.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      s_gles2.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_externalFrameWidth,
                          m_externalFrameHeight, 0, m_externalFrameFormat,
                          m_externalFrameType, m_externalFramePixels.data());
      m_externalUploadedGeneration = m_externalFrameGeneration;
    }
    // TextureDraw's base coordinates already account for gfxstream's
    // bottom-to-top (ydir=-1) readback. Only invert explicitly top-to-bottom
    // buffers here.
    m_textureDraw->draw(m_externalFrameTexture,
                        m_externalFrameYDirection > 0);
  } else if (!m_externalFrameSource) {
    for (const auto &r : renderables)
      draw(w->second, r,
           r.alpha() < 1.0f ? m_alphaProgram : m_defaultProgram);
  }

  if (sample_presentation && !renderables.empty()) {
    GLubyte texture_rgba[4] = {};
    GLubyte framebuffer_rgba[4] = {};
    const auto color_buffer = m_colorbuffers.find(renderables.front().buffer());
    const bool sampled_texture = color_buffer != m_colorbuffers.end() &&
        color_buffer->second.cb->sampleCurrentTexture(texture_rgba);
    s_gles2.glReadPixels(surface_width / 2, surface_height / 2,
                         1, 1, GL_RGBA, GL_UNSIGNED_BYTE, framebuffer_rgba);
    WARNING("Presentation samples: buffer=%#x sampled=%d tr=%d tg=%d tb=%d ta=%d fr=%d fg=%d fb=%d fa=%d",
            renderables.front().buffer(), sampled_texture,
            static_cast<int>(texture_rgba[0]), static_cast<int>(texture_rgba[1]),
            static_cast<int>(texture_rgba[2]), static_cast<int>(texture_rgba[3]),
            static_cast<int>(framebuffer_rgba[0]),
            static_cast<int>(framebuffer_rgba[1]),
            static_cast<int>(framebuffer_rgba[2]),
            static_cast<int>(framebuffer_rgba[3]));
  }

  const EGLBoolean swapped = s_egl.eglSwapBuffers(m_eglDisplay, w->second->surface);
  EGLint swap_error = EGL_SUCCESS;
  if (!swapped) {
    swap_error = s_egl.eglGetError();
    ERROR("Presentation trace: host eglSwapBuffers failed error=%#x", swap_error);
  }

  unbind_locked();

  if (!swapped && (swap_error == EGL_BAD_SURFACE ||
                   swap_error == EGL_BAD_NATIVE_WINDOW))
    recreateWindowSurfaceLocked(w->second);

  return swapped == EGL_TRUE;
}

void Renderer::enableExternalFrameSource(int width, int height) {
  std::lock_guard<std::mutex> lock(m_lock);
  m_externalFrameSource = true;
  m_externalExpectedWidth = width;
  m_externalExpectedHeight = height;
  m_externalFramePixels.clear();
  m_externalFrameWidth = 0;
  m_externalFrameHeight = 0;
  m_externalFrameGeneration = 0;
  m_externalUploadedGeneration = 0;
}

void Renderer::disableExternalFrameSource() {
  std::lock_guard<std::mutex> lock(m_lock);
  m_externalFrameSource = false;
  m_externalFramePixels.clear();
  m_externalFrameWidth = 0;
  m_externalFrameHeight = 0;
}

void Renderer::postExternalFrame(int width, int height, int y_direction,
                                 int format, int type,
                                 const unsigned char* pixels) {
  if (!pixels || width <= 0 || height <= 0 || format != GL_RGBA ||
      type != GL_UNSIGNED_BYTE) {
    WARNING("gfxstream frame rejected: size=%dx%d y=%d format=%#x type=%#x",
            width, height, y_direction, format, type);
    return;
  }
  constexpr std::size_t max_frame_bytes = 64U * 1024U * 1024U;
  const auto bytes = static_cast<std::size_t>(width) *
                     static_cast<std::size_t>(height) * 4U;
  if (bytes > max_frame_bytes) {
    WARNING("gfxstream frame rejected: %zu bytes exceeds %zu-byte limit",
            bytes, max_frame_bytes);
    return;
  }

  std::function<void()> redraw;
  std::uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(m_lock);
    if (!m_externalFrameSource)
      return;
    m_externalFramePixels.assign(pixels, pixels + bytes);
    m_externalFrameWidth = width;
    m_externalFrameHeight = height;
    m_externalFrameYDirection = y_direction;
    m_externalFrameFormat = format;
    m_externalFrameType = type;
    generation = ++m_externalFrameGeneration;
    redraw = m_redrawRequester;
  }
  if ((generation % 120U) == 1U)
    INFO("gfxstream frame: generation=%llu size=%dx%d bytes=%zu queue_depth=1 y=%d",
         static_cast<unsigned long long>(generation), width, height, bytes,
         y_direction);
  if (redraw)
    redraw();
}

bool Renderer::retain_frame(RenderableList &renderables) {
  constexpr size_t max_color_buffer_bytes = 2048U * 1024U * 1024U;
  constexpr size_t max_color_buffers = 128U;
  std::unique_lock<std::recursive_mutex> host_gles_lock(
      anbox::graphics::OpenGlesMessageProcessor::host_gles_lock());
  std::unique_lock<std::mutex> lock(m_lock);

  std::map<HandleType, HandleType> snapshots;
  std::vector<HandleType> created;
  auto rollback = [&] {
    for (const auto handle : created) {
      auto entry = m_colorbuffers.find(handle);
      if (entry == m_colorbuffers.end())
        continue;
      m_colorBufferBytes -= entry->second.allocation_bytes;
      m_colorbuffers.erase(entry);
    }
  };

  for (const auto &renderable : renderables) {
    const HandleType source_handle = renderable.buffer();
    if (snapshots.find(source_handle) != snapshots.end())
      continue;
    const auto source = m_colorbuffers.find(source_handle);
    if (source == m_colorbuffers.end()) {
      rollback();
      return false;
    }

    const auto width = static_cast<size_t>(source->second.width);
    const auto height = static_cast<size_t>(source->second.height);
    if (width > std::numeric_limits<size_t>::max() / height / 8U) {
      rollback();
      return false;
    }
    const size_t allocation_bytes = width * height * 8U;
    if (m_colorbuffers.size() >= max_color_buffers ||
        allocation_bytes > max_color_buffer_bytes ||
        m_colorBufferBytes > max_color_buffer_bytes - allocation_bytes) {
      ERROR("Frame snapshot rejected: source=%#x size=%zux%zu buffers=%zu bytes=%zu",
            source_handle, width, height, m_colorbuffers.size(),
            m_colorBufferBytes);
      rollback();
      return false;
    }

    ColorBufferPtr snapshot(ColorBuffer::create(
        getDisplay(), source->second.width, source->second.height,
        source->second.format, false, m_colorBufferHelper));
    if (!snapshot || !snapshot->snapshotFrom(*source->second.cb)) {
      ERROR("Frame snapshot failed: source=%#x size=%zux%zu", source_handle,
            width, height);
      rollback();
      return false;
    }

    const HandleType snapshot_handle = genHandle();
    auto &entry = m_colorbuffers[snapshot_handle];
    entry.cb = std::move(snapshot);
    entry.refcount = 1;
    entry.allocation_bytes = allocation_bytes;
    entry.width = source->second.width;
    entry.height = source->second.height;
    entry.format = source->second.format;
    m_colorBufferBytes += allocation_bytes;
    snapshots[source_handle] = snapshot_handle;
    created.push_back(snapshot_handle);
  }

  for (auto &renderable : renderables)
    renderable.set_buffer(snapshots.at(renderable.buffer()));

  static std::uint64_t snapshot_sequence = 0;
  if ((++snapshot_sequence % 60U) == 1U)
    INFO("Frame snapshot: sequence=%llu source_buffers=%zu immutable_buffers=%zu live_bytes=%zu",
         static_cast<unsigned long long>(snapshot_sequence), snapshots.size(),
         created.size(), m_colorBufferBytes);
  return true;
}

void Renderer::release_frame(const RenderableList &renderables) {
  std::unique_lock<std::mutex> lock(m_lock);
  std::set<HandleType> released;
  for (const auto &renderable : renderables) {
    const HandleType handle = renderable.buffer();
    if (!released.insert(handle).second)
      continue;
    auto entry = m_colorbuffers.find(handle);
    if (entry == m_colorbuffers.end())
      continue;
    if (entry->second.refcount == 0) {
      ERROR("Queued frame ColorBuffer %#x reference underflow prevented", handle);
      continue;
    }
    if (--entry->second.refcount == 0) {
      m_colorBufferBytes -= entry->second.allocation_bytes;
      m_colorbuffers.erase(entry);
    }
  }
}
