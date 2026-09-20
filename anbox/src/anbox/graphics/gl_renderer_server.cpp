/*
 * Copyright (C) 2016 Simon Fels <morphis@gravedo.de>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranties of
 * MERCHANTABILITY, SATISFACTORY QUALITY, or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "anbox/graphics/gl_renderer_server.h"
#include "anbox/graphics/emugl/RenderApi.h"
#include "anbox/graphics/emugl/RenderControl.h"
#include "anbox/graphics/emugl/Renderer.h"
#include "anbox/graphics/layer_composer.h"
#include "anbox/graphics/gfxstream_backend.h"
#include "anbox/graphics/multi_window_composer_strategy.h"
#include "anbox/graphics/single_window_composer_strategy.h"
#include "anbox/logger.h"
#include "anbox/wm/manager.h"

#include <boost/throw_exception.hpp>
#include <boost/filesystem.hpp>
#include <cstdarg>
#include <stdexcept>

namespace fs = boost::filesystem;

namespace {
void logger_write(const emugl::LogLevel &level, const char *format, ...) {
  (void)level;

  char message[2048];
  va_list args;

  va_start(args, format);
  vsnprintf(message, sizeof(message) - 1, format, args);
  va_end(args);

  switch (level) {
  case emugl::LogLevel::WARNING:
    WARNING("%s", message);
    break;
  case emugl::LogLevel::ERROR:
    ERROR("%s", message);
    break;
  case emugl::LogLevel::FATAL:
    FATAL("%s", message);
    break;
  case emugl::LogLevel::DEBUG:
    DEBUG("%s", message);
    break;
  case emugl::LogLevel::TRACE:
    TRACE("%s", message);
    break;
  default:
    break;
  }
}
}

namespace anbox::graphics {
GLRendererServer::GLRendererServer(const Config &config, const std::shared_ptr<wm::Manager> &wm)
    : renderer_(std::make_shared<::Renderer>()) {

  std::shared_ptr<LayerComposer::Strategy> composer_strategy;
  if (config.single_window)
    composer_strategy = std::make_shared<SingleWindowComposerStrategy>(wm);
  else
    composer_strategy = std::make_shared<MultiWindowComposerStrategy>(wm);

  const auto composer_mode = config.backend == Config::Backend::Legacy
      ? LayerComposer::Mode::LegacySynchronous
      : LayerComposer::Mode::LatestFrame;
  composer_ = std::make_shared<LayerComposer>(renderer_, composer_strategy,
                                              composer_mode);

  auto gl_libs = emugl::default_gl_libraries();
  if (config.driver == Config::Driver::Software) {
    auto swiftshader_path = fs::path(utils::get_env_value("SWIFTSHADER_PATH"));
    const auto snap_path = utils::get_env_value("SNAP");
    if (!snap_path.empty())
      swiftshader_path = fs::path(snap_path) / "lib" / "anbox" / "swiftshader";
    if (!fs::exists(swiftshader_path))
      throw std::runtime_error("Software rendering is enabled, but SwiftShader library directory is not found.");

    gl_libs = std::vector<emugl::GLLibrary>{
      {emugl::GLLibrary::Type::EGL, (swiftshader_path / "libEGL.so").string()},
      {emugl::GLLibrary::Type::GLESv1, (swiftshader_path / "libGLES_CM.so").string()},
      {emugl::GLLibrary::Type::GLESv2, (swiftshader_path / "libGLESv2.so").string()},
    };
  }

  emugl_logger_struct log_funcs;
  log_funcs.coarse = logger_write;
  log_funcs.fine = logger_write;

  if (!emugl::initialize(gl_libs, &log_funcs, nullptr))
    BOOST_THROW_EXCEPTION(std::runtime_error("Failed to initialize OpenGL renderer"));

  if (!renderer_->initialize(0))
    throw std::runtime_error("Failed to initialize EGL renderer");
  if (config.driver == Config::Driver::Host && renderer_->isSoftwareRenderer() &&
      !config.allow_software_fallback) {
    throw std::runtime_error(
        "Host EGL selected a software rasterizer; use --software-rendering "
        "or explicitly opt in with --allow-software-fallback");
  }

  INFO("Renderer backend=%s driver_request=%s acceleration=%s queue_capacity=%d",
       config.backend == Config::Backend::Legacy ? "legacy" : "reboxed",
       config.driver == Config::Driver::Software ? "software" : "host",
       renderer_->isSoftwareRenderer() ? "software" : "hardware",
       config.backend == Config::Backend::Legacy ? 0 : 1);
  INFO("Renderer synchronization: guest EGL fences complete through host glFinish; presentation transports ColorBuffer handles, not native fence FDs");

  if (config.use_gfxstream) {
    gfxstream_ = std::make_shared<GfxstreamBackend>(
        renderer_, config.android_width, config.android_height,
        config.require_vulkan);
  }

  registerRenderer(renderer_);
  registerLayerComposer(composer_);
}

GLRendererServer::~GLRendererServer() {
  // Stop presentation before destroying EGL objects it may be using.
  registerLayerComposer(nullptr);
  composer_.reset();
  gfxstream_.reset();
  registerRenderer(nullptr);
  renderer_->finalize();
}
}
