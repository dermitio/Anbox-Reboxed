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

#include "anbox/graphics/emugl/RenderContext.h"
#include "anbox/logger.h"

#include "OpenGLESDispatch/EGLDispatch.h"

RenderContext* RenderContext::create(EGLDisplay display, EGLConfig config,
                                     EGLContext sharedContext,
                                     unsigned int guestGlesVersion) {
  // EGL's bound client API is per-thread.  Renderer::initialize() binds GLES
  // on the presentation thread, but guest contexts are created on individual
  // RenderThreads.  Relying on inherited/default EGL state makes the context
  // attributes get interpreted for the wrong API on some drivers (Mesa
  // reports EGL_BAD_ATTRIBUTE for EGL_CONTEXT_CLIENT_VERSION in that case).
  if (!s_egl.eglBindAPI(EGL_OPENGL_ES_API)) {
    ERROR("Context lifecycle: failed to bind the OpenGL ES API error=0x%x",
          s_egl.eglGetError());
    return NULL;
  }

  const bool isGl2 = guestGlesVersion >= 2;
  const EGLint hostGlesVersion = guestGlesVersion >= 3 ? 3 : (isGl2 ? 2 : 1);
  const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, hostGlesVersion,
                                   EGL_NONE};
  EGLContext context =
      s_egl.eglCreateContext(display, config, sharedContext, contextAttribs);
  if (context == EGL_NO_CONTEXT) {
    const auto error = s_egl.eglGetError();
    EGLint config_id = 0;
    EGLint renderable_type = 0;
    const EGLBoolean config_id_ok =
        s_egl.eglGetConfigAttrib(display, config, EGL_CONFIG_ID, &config_id);
    const EGLBoolean renderable_ok = s_egl.eglGetConfigAttrib(
        display, config, EGL_RENDERABLE_TYPE, &renderable_type);
    const char* vendor = s_egl.eglQueryString(display, EGL_VENDOR);
    const EGLint query_error = s_egl.eglGetError();
    const EGLenum bound_api = s_egl.eglQueryAPI();
    ERROR("Context lifecycle: host eglCreateContext failed requested_gles=%d shared=%p error=0x%x config=%d renderable=%#x bound_api=%#x",
          hostGlesVersion, reinterpret_cast<void *>(sharedContext), error,
          config_id, renderable_type, bound_api);
    ERROR("Context lifecycle: diagnostic config_id_ok=%u renderable_ok=%u vendor='%s' query_error=%#x",
          config_id_ok, renderable_ok, vendor ? vendor : "null", query_error);
    return NULL;
  }

  return new RenderContext(display, context, isGl2, guestGlesVersion);
}

RenderContext::RenderContext(EGLDisplay display, EGLContext context, bool isGl2,
                             unsigned int guestGlesVersion)
    : mDisplay(display),
      mContext(context),
      mIsGl2(isGl2),
      mGuestGlesVersion(guestGlesVersion),
      mContextData() {}

RenderContext::~RenderContext() {
  if (mContext != EGL_NO_CONTEXT) {
    s_egl.eglDestroyContext(mDisplay, mContext);
  }
}
