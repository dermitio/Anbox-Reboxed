/*
* Copyright (C) 2015 The Android Open Source Project
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

#ifndef _LIBRENDER_TEXTURERESIZE_H
#define _LIBRENDER_TEXTURERESIZE_H

#include <GLES2/gl2.h>

class TextureResize {
 public:
  TextureResize(GLuint width, GLuint height);
  ~TextureResize();

  // Scales the given texture for the current viewport and returns the scaled
  // texture. May return the input if no scaling is required.
  GLuint update(GLuint texture);

  // Pure geometry portion of update(), exposed for lifecycle tests. The
  // result is always a power of two and never downsamples below the target.
  static unsigned int calculateFactor(GLuint width, GLuint height,
                                      GLint target_width,
                                      GLint target_height);

  struct Framebuffer {
    GLuint texture = 0;
    GLuint framebuffer = 0;
    GLuint program = 0;
    GLint aPosition = -1;
    GLint uTexture = -1;
  };

 private:
  bool setupFramebuffers(unsigned int factor);
  bool resize(GLuint texture);

 private:
  GLuint mWidth;
  GLuint mHeight;
  unsigned int mFactor;
  Framebuffer mFBWidth;
  Framebuffer mFBHeight;
  GLuint mVertexBuffer;
};

#endif
