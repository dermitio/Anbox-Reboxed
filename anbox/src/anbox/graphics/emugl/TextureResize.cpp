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

#include "anbox/graphics/emugl/TextureResize.h"
#include "anbox/graphics/emugl/DispatchTables.h"
#include "anbox/logger.h"

#include <stdio.h>
#include <sstream>
#include <string>
#include <utility>

#define MAX_FACTOR_POWER 4

static const char kCommonShaderSource[] =
    "precision mediump float;\n"
    "varying vec2 vUV00, vUV01;\n"
    "#if FACTOR > 2\n"
    "varying vec2 vUV02, vUV03;\n"
    "#if FACTOR > 4\n"
    "varying vec2 vUV04, vUV05, vUV06, vUV07;\n"
    "#if FACTOR > 8\n"
    "varying vec2 vUV08, vUV09, vUV10, vUV11, vUV12, vUV13, vUV14, vUV15;\n"
    "#endif\n"
    "#endif\n"
    "#endif\n";

static const char kVertexShaderSource[] =
    "attribute vec2 aPosition;\n"

    "void main() {\n"
    "  gl_Position = vec4(aPosition, 0, 1);\n"
    "  vec2 uv = ((aPosition + 1.0) / 2.0) + 0.5 / kDimension;\n"
    "  vUV00 = uv;\n"
    "  #ifdef HORIZONTAL\n"
    "  vUV01 = uv + vec2( 1.0 / kDimension.x, 0);\n"
    "  #if FACTOR > 2\n"
    "  vUV02 = uv + vec2( 2.0 / kDimension.x, 0);\n"
    "  vUV03 = uv + vec2( 3.0 / kDimension.x, 0);\n"
    "  #if FACTOR > 4\n"
    "  vUV04 = uv + vec2( 4.0 / kDimension.x, 0);\n"
    "  vUV05 = uv + vec2( 5.0 / kDimension.x, 0);\n"
    "  vUV06 = uv + vec2( 6.0 / kDimension.x, 0);\n"
    "  vUV07 = uv + vec2( 7.0 / kDimension.x, 0);\n"
    "  #if FACTOR > 8\n"
    "  vUV08 = uv + vec2( 8.0 / kDimension.x, 0);\n"
    "  vUV09 = uv + vec2( 9.0 / kDimension.x, 0);\n"
    "  vUV10 = uv + vec2(10.0 / kDimension.x, 0);\n"
    "  vUV11 = uv + vec2(11.0 / kDimension.x, 0);\n"
    "  vUV12 = uv + vec2(12.0 / kDimension.x, 0);\n"
    "  vUV13 = uv + vec2(13.0 / kDimension.x, 0);\n"
    "  vUV14 = uv + vec2(14.0 / kDimension.x, 0);\n"
    "  vUV15 = uv + vec2(15.0 / kDimension.x, 0);\n"
    "  #endif\n"  // FACTOR > 8
    "  #endif\n"  // FACTOR > 4
    "  #endif\n"  // FACTOR > 2

    "  #else\n"
    "  vUV01 = uv + vec2(0,  1.0 / kDimension.y);\n"
    "  #if FACTOR > 2\n"
    "  vUV02 = uv + vec2(0,  2.0 / kDimension.y);\n"
    "  vUV03 = uv + vec2(0,  3.0 / kDimension.y);\n"
    "  #if FACTOR > 4\n"
    "  vUV04 = uv + vec2(0,  4.0 / kDimension.y);\n"
    "  vUV05 = uv + vec2(0,  5.0 / kDimension.y);\n"
    "  vUV06 = uv + vec2(0,  6.0 / kDimension.y);\n"
    "  vUV07 = uv + vec2(0,  7.0 / kDimension.y);\n"
    "  #if FACTOR > 8\n"
    "  vUV08 = uv + vec2(0,  8.0 / kDimension.y);\n"
    "  vUV09 = uv + vec2(0,  9.0 / kDimension.y);\n"
    "  vUV10 = uv + vec2(0, 10.0 / kDimension.y);\n"
    "  vUV11 = uv + vec2(0, 11.0 / kDimension.y);\n"
    "  vUV12 = uv + vec2(0, 12.0 / kDimension.y);\n"
    "  vUV13 = uv + vec2(0, 13.0 / kDimension.y);\n"
    "  vUV14 = uv + vec2(0, 14.0 / kDimension.y);\n"
    "  vUV15 = uv + vec2(0, 15.0 / kDimension.y);\n"
    "  #endif\n"  // FACTOR > 8
    "  #endif\n"  // FACTOR > 4
    "  #endif\n"  // FACTOR > 2
    "  #endif\n"  // HORIZONTAL/VERTICAL
    "}\n";

const char kFragmentShaderSource[] =
    "uniform sampler2D uTexture;\n"

    "vec4 read(vec2 uv) {\n"
    "  vec4 r = texture2D(uTexture, uv);\n"
    "  #ifdef HORIZONTAL\n"
    "  r.rgb = pow(r.rgb, vec3(2.2));\n"
    "  #endif\n"
    "  return r;\n"
    "}\n"

    "void main() {\n"
    "  vec4 sum = read(vUV00) + read(vUV01);\n"
    "  #if FACTOR > 2\n"
    "  sum += read(vUV02) + read(vUV03);\n"
    "  #if FACTOR > 4\n"
    "  sum += read(vUV04) + read(vUV05) + read(vUV06) + read(vUV07);\n"
    "  #if FACTOR > 8\n"
    "  sum += read(vUV08) + read(vUV09) + read(vUV10) + read(vUV11) +"
    "      read(vUV12) + read(vUV13) + read(vUV14) + read(vUV15);\n"
    "  #endif\n"
    "  #endif\n"
    "  #endif\n"
    "  sum /= float(FACTOR);\n"
    "  #ifdef VERTICAL\n"
    "  sum.rgb = pow(sum.rgb, vec3(1.0 / 2.2));\n"
    "  #endif\n"
    "  gl_FragColor = sum;\n"
    "}\n";

static const float kVertexData[] = {-1, -1, 3, -1, -1, 3};

namespace {

const char* framebufferStatusName(GLenum status) {
  switch (status) {
    case GL_FRAMEBUFFER_COMPLETE:
      return "complete";
    case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
      return "incomplete attachment";
    case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:
      return "missing attachment";
    case GL_FRAMEBUFFER_UNSUPPORTED:
      return "unsupported";
    default:
      return "unknown";
  }
}

bool checkFramebuffer(const char* pass, GLuint framebuffer, GLuint texture,
                      GLuint width, GLuint height) {
  const GLenum status = s_gles2.glCheckFramebufferStatus(GL_FRAMEBUFFER);
  const GLenum error = s_gles2.glGetError();
  if (status == GL_FRAMEBUFFER_COMPLETE && error == GL_NO_ERROR)
    return true;

  ERROR("TextureResize %s framebuffer invalid: fbo=%u texture=%u target=GL_TEXTURE_2D format=GL_RGBA type=GL_UNSIGNED_BYTE size=%ux%u status=%#x (%s) error=%#x",
        pass, framebuffer, texture, width, height, status,
        framebufferStatusName(status), error);
  return false;
}

void restoreCapability(GLenum capability, GLboolean enabled) {
  if (enabled)
    s_gles2.glEnable(capability);
  else
    s_gles2.glDisable(capability);
}

void restoreAttribute(GLint attribute, GLint enabled) {
  if (enabled)
    s_gles2.glEnableVertexAttribArray(attribute);
  else
    s_gles2.glDisableVertexAttribArray(attribute);
}

}  // namespace

static void detachShaders(GLuint program) {
  GLuint shaders[2] = {};
  GLsizei count = 0;
  s_gles2.glGetAttachedShaders(program, 2, &count, shaders);
  if (s_gles2.glGetError() == GL_NO_ERROR) {
    for (GLsizei i = 0; i < count; i++) {
      s_gles2.glDetachShader(program, shaders[i]);
      s_gles2.glDeleteShader(shaders[i]);
    }
  }
}

static GLuint createShader(GLenum type,
                           const std::initializer_list<const char*>& source) {
  GLint success, infoLength;

  GLuint shader = s_gles2.glCreateShader(type);
  if (shader) {
    s_gles2.glShaderSource(shader, source.size(), source.begin(), nullptr);
    s_gles2.glCompileShader(shader);
    s_gles2.glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (success == GL_FALSE) {
      s_gles2.glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLength);
      std::string infoLog(infoLength + 1, '\0');
      s_gles2.glGetShaderInfoLog(shader, infoLength, nullptr, &infoLog[0]);
      ERROR("%s shader compile failed: %s", (type == GL_VERTEX_SHADER) ? "Vertex" : "Fragment", infoLog.c_str());
      s_gles2.glDeleteShader(shader);
      shader = 0;
    }
  }
  return shader;
}

static void attachShaders(TextureResize::Framebuffer* fb,
                          const char* factorDefine, const char* dimensionDefine,
                          GLuint width, GLuint height) {
  std::ostringstream dimensionConst;
  dimensionConst << "const vec2 kDimension = vec2(" << width << ", " << height
                 << ");\n";

  GLuint vShader = createShader(
      GL_VERTEX_SHADER, {factorDefine, dimensionDefine, kCommonShaderSource,
                         dimensionConst.str().c_str(), kVertexShaderSource});
  GLuint fShader = createShader(
      GL_FRAGMENT_SHADER, {factorDefine, dimensionDefine, kCommonShaderSource,
                           kFragmentShaderSource});

  if (!vShader || !fShader) {
    return;
  }

  s_gles2.glAttachShader(fb->program, vShader);
  s_gles2.glAttachShader(fb->program, fShader);
  s_gles2.glLinkProgram(fb->program);

  s_gles2.glUseProgram(fb->program);
  fb->aPosition = s_gles2.glGetAttribLocation(fb->program, "aPosition");
  fb->uTexture = s_gles2.glGetUniformLocation(fb->program, "uTexture");
}

TextureResize::TextureResize(GLuint width, GLuint height)
    : mWidth(width),
      mHeight(height),
      mFactor(1) {
  s_gles2.glGenTextures(1, &mFBWidth.texture);
  s_gles2.glBindTexture(GL_TEXTURE_2D, mFBWidth.texture);
  s_gles2.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  s_gles2.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  s_gles2.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  s_gles2.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  s_gles2.glGenTextures(1, &mFBHeight.texture);
  s_gles2.glBindTexture(GL_TEXTURE_2D, mFBHeight.texture);
  s_gles2.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  s_gles2.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  s_gles2.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  s_gles2.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  s_gles2.glGenFramebuffers(1, &mFBWidth.framebuffer);
  s_gles2.glGenFramebuffers(1, &mFBHeight.framebuffer);

  mFBWidth.program = s_gles2.glCreateProgram();
  mFBHeight.program = s_gles2.glCreateProgram();

  s_gles2.glGenBuffers(1, &mVertexBuffer);
  s_gles2.glBindBuffer(GL_ARRAY_BUFFER, mVertexBuffer);
  s_gles2.glBufferData(GL_ARRAY_BUFFER, sizeof(kVertexData), kVertexData,
                       GL_STATIC_DRAW);
}

TextureResize::~TextureResize() {
  GLuint fb[2] = {mFBWidth.framebuffer, mFBHeight.framebuffer};
  s_gles2.glDeleteFramebuffers(2, fb);

  GLuint tex[2] = {mFBWidth.texture, mFBHeight.texture};
  s_gles2.glDeleteTextures(2, tex);

  s_gles2.glDeleteProgram(mFBWidth.program);
  s_gles2.glDeleteProgram(mFBHeight.program);

  s_gles2.glDeleteBuffers(1, &mVertexBuffer);
}

unsigned int TextureResize::calculateFactor(GLuint width, GLuint height,
                                            GLint target_width,
                                            GLint target_height) {
  if (target_width <= 0 || target_height <= 0)
    return 1;

  unsigned int factor = 1;
  for (int i = 0, w = width / 2, h = height / 2;
       i < MAX_FACTOR_POWER && w >= target_width && h >= target_height;
       i++, w /= 2, h /= 2, factor *= 2) {
  }
  return factor;
}

GLuint TextureResize::update(GLuint texture) {
  // TextureResize runs inside the presentation context. Preserve every piece
  // of mutable state used below so the caller cannot accidentally draw the
  // final layer with a resize shader, intermediate framebuffer, or stale
  // scissor rectangle.
  GLint vport[4] = {};
  GLint framebuffer = 0;
  GLint array_buffer = 0;
  GLint program = 0;
  GLint active_texture = 0;
  GLint active_texture_binding = 0;
  GLint texture0_binding = 0;
  GLint scissor_box[4] = {};
  GLboolean color_mask[4] = {};
  const GLboolean scissor_enabled = s_gles2.glIsEnabled(GL_SCISSOR_TEST);
  const GLboolean blend_enabled = s_gles2.glIsEnabled(GL_BLEND);
  const GLboolean depth_enabled = s_gles2.glIsEnabled(GL_DEPTH_TEST);
  const GLboolean cull_enabled = s_gles2.glIsEnabled(GL_CULL_FACE);
  s_gles2.glGetIntegerv(GL_VIEWPORT, vport);
  s_gles2.glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
  s_gles2.glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer);
  s_gles2.glGetIntegerv(GL_CURRENT_PROGRAM, &program);
  s_gles2.glGetIntegerv(GL_ACTIVE_TEXTURE, &active_texture);
  s_gles2.glGetIntegerv(GL_TEXTURE_BINDING_2D, &active_texture_binding);
  s_gles2.glGetIntegerv(GL_SCISSOR_BOX, scissor_box);
  s_gles2.glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
  if (active_texture != GL_TEXTURE0) {
    s_gles2.glActiveTexture(GL_TEXTURE0);
    s_gles2.glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture0_binding);
    s_gles2.glActiveTexture(active_texture);
  } else {
    texture0_binding = active_texture_binding;
  }

  // Correctly deal with rotated screens.
  GLint tWidth = vport[2], tHeight = vport[3];
  if ((mWidth < mHeight) != (tWidth < tHeight)) {
    std::swap(tWidth, tHeight);
  }

  // Compute the scaling factor needed to get an image just larger than the
  // target viewport.
  const unsigned int factor =
      calculateFactor(mWidth, mHeight, tWidth, tHeight);

  // No resizing needed.
  if (factor == 1) {
    return texture;
  }

  // A scissor or fixed-function test inherited from a guest operation must
  // not constrain either downsample pass.
  s_gles2.glDisable(GL_SCISSOR_TEST);
  s_gles2.glDisable(GL_BLEND);
  s_gles2.glDisable(GL_DEPTH_TEST);
  s_gles2.glDisable(GL_CULL_FACE);
  s_gles2.glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

  bool resized = false;
  if (setupFramebuffers(factor)) {
    GLint width_attribute_enabled = GL_FALSE;
    GLint height_attribute_enabled = GL_FALSE;
    if (mFBWidth.aPosition >= 0)
      s_gles2.glGetVertexAttribiv(mFBWidth.aPosition,
                                 GL_VERTEX_ATTRIB_ARRAY_ENABLED,
                                 &width_attribute_enabled);
    if (mFBHeight.aPosition >= 0 &&
        mFBHeight.aPosition != mFBWidth.aPosition)
      s_gles2.glGetVertexAttribiv(mFBHeight.aPosition,
                                 GL_VERTEX_ATTRIB_ARRAY_ENABLED,
                                 &height_attribute_enabled);

    resized = resize(texture);

    if (mFBWidth.aPosition >= 0)
      restoreAttribute(mFBWidth.aPosition, width_attribute_enabled);
    if (mFBHeight.aPosition >= 0 &&
        mFBHeight.aPosition != mFBWidth.aPosition)
      restoreAttribute(mFBHeight.aPosition, height_attribute_enabled);
  }

  s_gles2.glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  s_gles2.glViewport(vport[0], vport[1], vport[2], vport[3]);
  s_gles2.glUseProgram(program);
  s_gles2.glBindBuffer(GL_ARRAY_BUFFER, array_buffer);
  s_gles2.glActiveTexture(GL_TEXTURE0);
  s_gles2.glBindTexture(GL_TEXTURE_2D, texture0_binding);
  if (active_texture != GL_TEXTURE0) {
    s_gles2.glActiveTexture(active_texture);
    s_gles2.glBindTexture(GL_TEXTURE_2D, active_texture_binding);
  }
  s_gles2.glScissor(scissor_box[0], scissor_box[1], scissor_box[2],
                    scissor_box[3]);
  s_gles2.glColorMask(color_mask[0], color_mask[1], color_mask[2],
                      color_mask[3]);
  restoreCapability(GL_SCISSOR_TEST, scissor_enabled);
  restoreCapability(GL_BLEND, blend_enabled);
  restoreCapability(GL_DEPTH_TEST, depth_enabled);
  restoreCapability(GL_CULL_FACE, cull_enabled);

  return resized ? mFBHeight.texture : texture;
}

bool TextureResize::setupFramebuffers(unsigned int factor) {
  if (factor == mFactor) {
    // The factor hasn't changed, no need to update the framebuffers.
    s_gles2.glBindFramebuffer(GL_FRAMEBUFFER, mFBWidth.framebuffer);
    if (!checkFramebuffer("horizontal", mFBWidth.framebuffer,
                          mFBWidth.texture, mWidth / factor, mHeight))
      return false;
    s_gles2.glBindFramebuffer(GL_FRAMEBUFFER, mFBHeight.framebuffer);
    return checkFramebuffer("vertical", mFBHeight.framebuffer,
                            mFBHeight.texture, mWidth / factor,
                            mHeight / factor);
  }

  // Update the framebuffer sizes to match the new factor.
  s_gles2.glBindTexture(GL_TEXTURE_2D, mFBWidth.texture);
  s_gles2.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, mWidth / factor, mHeight, 0,
                       GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  GLenum error = s_gles2.glGetError();
  if (error != GL_NO_ERROR) {
    ERROR("TextureResize horizontal texture allocation failed: texture=%u size=%ux%u format=GL_RGBA type=GL_UNSIGNED_BYTE error=%#x",
          mFBWidth.texture, mWidth / factor, mHeight, error);
    return false;
  }
  s_gles2.glBindFramebuffer(GL_FRAMEBUFFER, mFBWidth.framebuffer);
  s_gles2.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                 GL_TEXTURE_2D, mFBWidth.texture, 0);
  if (!checkFramebuffer("horizontal", mFBWidth.framebuffer, mFBWidth.texture,
                        mWidth / factor, mHeight))
    return false;

  s_gles2.glBindTexture(GL_TEXTURE_2D, mFBHeight.texture);
  s_gles2.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, mWidth / factor,
                       mHeight / factor, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  error = s_gles2.glGetError();
  if (error != GL_NO_ERROR) {
    ERROR("TextureResize vertical texture allocation failed: texture=%u size=%ux%u format=GL_RGBA type=GL_UNSIGNED_BYTE error=%#x",
          mFBHeight.texture, mWidth / factor, mHeight / factor, error);
    return false;
  }
  s_gles2.glBindFramebuffer(GL_FRAMEBUFFER, mFBHeight.framebuffer);
  s_gles2.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                 GL_TEXTURE_2D, mFBHeight.texture, 0);
  if (!checkFramebuffer("vertical", mFBHeight.framebuffer, mFBHeight.texture,
                        mWidth / factor, mHeight / factor))
    return false;

  // Update the shaders to the new factor. First detach the old shaders...
  detachShaders(mFBWidth.program);
  detachShaders(mFBHeight.program);

  // ... then attach the new ones.
  std::ostringstream factorDefine;
  factorDefine << "#define FACTOR " << factor << "\n";
  attachShaders(&mFBWidth, factorDefine.str().c_str(), "#define HORIZONTAL\n",
                mWidth, mHeight);
  attachShaders(&mFBHeight, factorDefine.str().c_str(), "#define VERTICAL\n",
                mWidth, mHeight);

  mFactor = factor;
  return true;
}

bool TextureResize::resize(GLuint texture) {
  s_gles2.glBindBuffer(GL_ARRAY_BUFFER, mVertexBuffer);
  s_gles2.glActiveTexture(GL_TEXTURE0);

  // First scale the horizontal dimension by rendering the input texture to a
  // scaled framebuffer.
  s_gles2.glBindFramebuffer(GL_FRAMEBUFFER, mFBWidth.framebuffer);
  if (!checkFramebuffer("horizontal draw", mFBWidth.framebuffer,
                        mFBWidth.texture, mWidth / mFactor, mHeight))
    return false;
  s_gles2.glViewport(0, 0, mWidth / mFactor, mHeight);
  s_gles2.glUseProgram(mFBWidth.program);
  s_gles2.glEnableVertexAttribArray(mFBWidth.aPosition);
  s_gles2.glVertexAttribPointer(mFBWidth.aPosition, 2, GL_FLOAT, GL_FALSE, 0,
                                0);
  s_gles2.glBindTexture(GL_TEXTURE_2D, texture);

  // Store the current texture filters and set to nearest for scaling.
  GLint mag_filter, min_filter;
  s_gles2.glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                              &mag_filter);
  s_gles2.glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                              &min_filter);
  s_gles2.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  s_gles2.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  s_gles2.glUniform1i(mFBWidth.uTexture, 0);
  s_gles2.glDrawArrays(GL_TRIANGLES, 0,
                       sizeof(kVertexData) / (2 * sizeof(float)));
  GLenum error = s_gles2.glGetError();

  // Restore the source texture even when the draw itself failed. A failed
  // resize must not mutate the guest ColorBuffer's sampling contract.
  s_gles2.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag_filter);
  s_gles2.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter);
  if (error != GL_NO_ERROR) {
    ERROR("TextureResize horizontal draw failed: source=%u destination=%u size=%ux%u error=%#x",
          texture, mFBWidth.texture, mWidth / mFactor, mHeight, error);
    return false;
  }

  // Secondly, scale the vertical dimension using the second framebuffer.
  s_gles2.glBindFramebuffer(GL_FRAMEBUFFER, mFBHeight.framebuffer);
  if (!checkFramebuffer("vertical draw", mFBHeight.framebuffer,
                        mFBHeight.texture, mWidth / mFactor,
                        mHeight / mFactor))
    return false;
  s_gles2.glViewport(0, 0, mWidth / mFactor, mHeight / mFactor);
  s_gles2.glUseProgram(mFBHeight.program);
  s_gles2.glEnableVertexAttribArray(mFBHeight.aPosition);
  s_gles2.glVertexAttribPointer(mFBHeight.aPosition, 2, GL_FLOAT, GL_FALSE, 0,
                                0);
  s_gles2.glBindTexture(GL_TEXTURE_2D, mFBWidth.texture);
  s_gles2.glUniform1i(mFBHeight.uTexture, 0);
  s_gles2.glDrawArrays(GL_TRIANGLES, 0,
                       sizeof(kVertexData) / (2 * sizeof(float)));
  error = s_gles2.glGetError();
  if (error != GL_NO_ERROR) {
    ERROR("TextureResize vertical draw failed: source=%u destination=%u size=%ux%u error=%#x",
          mFBWidth.texture, mFBHeight.texture, mWidth / mFactor,
          mHeight / mFactor, error);
    return false;
  }

  // Clear the bindings.
  s_gles2.glBindBuffer(GL_ARRAY_BUFFER, 0);
  s_gles2.glBindFramebuffer(GL_FRAMEBUFFER, 0);
  s_gles2.glBindTexture(GL_TEXTURE_2D, 0);
  return true;
}
