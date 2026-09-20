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

#include <boost/algorithm/string/split.hpp>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-default"
#include <sys/prctl.h>

#include <boost/algorithm/string.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/filesystem.hpp>

#include <cstdlib>
#include <chrono>
#include <sys/stat.h>

#include "anbox/application/database.h"
#include "anbox/application/launcher_storage.h"
#include "anbox/application/sensor_type.h"
#include "anbox/application/sensors_state.h"
#include "anbox/application/gps_info_broker.h"
#include "anbox/audio/server.h"
#include "anbox/bridge/android_api_stub.h"
#include "anbox/bridge/platform_api_skeleton.h"
#include "anbox/bridge/platform_message_processor.h"
#include "anbox/cmds/session_manager.h"
#include "anbox/common/binder_device_allocator.h"
#include "anbox/common/dispatcher.h"
#include "anbox/container/client.h"
#include "anbox/dbus/application_manager_server.h"
#include "anbox/dbus/gps_server.h"
#include "anbox/dbus/sensors_server.h"
#include "anbox/dbus/bus.h"
#include "anbox/dbus/interface.h"
#include "anbox/dbus/sensors_server.h"
#include "anbox/graphics/emugl/Renderer.h"
#include "anbox/graphics/emugl/DisplayManager.h"
#include "anbox/graphics/density.h"
#include "anbox/graphics/gl_renderer_server.h"
#include "anbox/graphics/gfxstream_backend.h"
#include "anbox/input/manager.h"
#include "anbox/logger.h"
#include "anbox/network/published_socket_connector.h"
#include "anbox/platform/base_platform.h"
#include "anbox/qemu/pipe_connection_creator.h"
#include "anbox/rpc/channel.h"
#include "anbox/rpc/connection_creator.h"
#include "anbox/runtime.h"
#include "anbox/system_configuration.h"
#include "anbox/wm/multi_window_manager.h"
#include "anbox/wm/single_window_manager.h"
#include "core/posix/signal.h"
#include "external/xdg/xdg.h"

#pragma GCC diagnostic pop

namespace fs = boost::filesystem;

namespace {
void prepare_runtime_directory(const std::string &path,
                               bool preserve_reboxed_bridge = false) {
  anbox::utils::ensure_paths({path});

  // Android init may have changed ownership/mode through the bind mount during
  // the previous boot. Restore host write access before removing stale files.
  ::chmod(path.c_str(), S_IRWXU | S_IRWXG | S_IRWXO);

  for (fs::directory_iterator it{path}, end; it != end; ++it) {
    if (preserve_reboxed_bridge &&
        it->path().filename() == "reboxed_bridge_v1") {
      struct stat info {};
      if (::lstat(it->path().c_str(), &info) == 0 && S_ISSOCK(info.st_mode) &&
          info.st_uid == ::getuid())
        continue;
    }
    fs::remove_all(it->path());
  }

  // These directories are bind-mounted into Android. A failed boot can leave
  // Android-owned socket files behind, so keep the directory writable by the
  // host session user for deterministic restart cleanup.
  ::chmod(path.c_str(), S_IRWXU | S_IRWXG | S_IRWXO);
}

constexpr const char *default_appmgr_package{"org.anbox.appmgr"};
constexpr const char *default_appmgr_component{"org.anbox.appmgr.AppViewActivity"};
constexpr std::chrono::milliseconds default_appmgr_startup_delay{50};

class NullConnectionCreator : public anbox::network::ConnectionCreator<
                                  boost::asio::local::stream_protocol> {
 public:
  void create_connection_for(
      std::shared_ptr<boost::asio::local::stream_protocol::socket> const
          &socket) override {
    WARNING("Not implemented");
    socket->close();
  }
};
}  // namespace

void anbox::cmds::SessionManager::launch_appmgr_if_needed(const std::shared_ptr<bridge::AndroidApiStub> &android_api_stub) {
  if (!single_window_)
    return;

  android::Intent launch_intent;
  launch_intent.package = default_appmgr_package;
  launch_intent.component = default_appmgr_component;
  // As this will only be executed in single window mode we don't have
  // to specify and launch bounds.
  android_api_stub->launch(launch_intent, graphics::Rect::Invalid, wm::Stack::Id::Default);
}

anbox::cmds::SessionManager::SessionManager()
    : CommandWithFlagsAndAction{cli::Name{"session-manager"}, cli::Usage{"session-manager"},
                                cli::Description{"Run the Anbox Reboxed session manager"}},
      window_size_(graphics::Rect::Invalid) {
  // Just for the purpose to allow QtMir (or unity8) to find this on our
  // /proc/*/cmdline
  // for proper confinement etc.
  flag(cli::make_flag(cli::Name{"desktop_file_hint"},
                      cli::Description{"Desktop file hint for QtMir/Unity8"},
                      desktop_file_hint_));
  flag(cli::make_flag(cli::Name{"single-window"},
                      cli::Description{"Start in single window mode."},
                      single_window_));
  flag(cli::make_flag(cli::Name{"window-size"},
                      cli::Description{"Exact host client size; omitted selects automatic portrait 20:9 sizing"},
                      window_size_));
  flag(cli::make_flag(cli::Name{"android-size"},
                      cli::Description{"Android framebuffer size (independent of host window size)"},
                      android_size_));
  flag(cli::make_flag(cli::Name{"density"},
                      cli::Description{"Android logical display density in DPI (reference: 320)"},
                      density_));
  flag(cli::make_flag(cli::Name{"orientation"},
                      cli::Description{"Native framebuffer orientation: auto, portrait, or landscape"},
                      orientation_));
  flag(cli::make_flag(cli::Name{"standalone"},
                      cli::Description{"Prevents the Container Manager from starting the default container (Experimental)"},
                      standalone_));
  flag(cli::make_flag(cli::Name{"experimental"},
                      cli::Description{"Allows users to use experimental features"},
                      experimental_));
  flag(cli::make_flag(cli::Name{"use-system-dbus"},
                      cli::Description{"Use system instead of session DBus"},
                      use_system_dbus_));
  flag(cli::make_flag(cli::Name{"no-dbus-service"},
                      cli::Description{"Do not publish the legacy org.anbox session-bus service"},
                      no_dbus_service_));
  flag(cli::make_flag(cli::Name{"software-rendering"},
                      cli::Description{"Use software rendering instead of hardware accelerated GL rendering"},
                      use_software_rendering_));
  flag(cli::make_flag(cli::Name{"renderer"},
                      cli::Description{"Presentation backend: reboxed or legacy"},
                      renderer_backend_));
  flag(cli::make_flag(
      cli::Name{"graphics-api"},
      cli::Description{
          "Android graphics transport: auto, vulkan (modern gfxstream), or gles (legacy compatibility)"},
      graphics_api_));
  flag(cli::make_flag(cli::Name{"allow-software-fallback"},
                      cli::Description{"Allow a host GL request to run on llvmpipe/softpipe/swrast"},
                      allow_software_fallback_));
  flag(cli::make_flag(cli::Name{"input-backend"},
                      cli::Description{"Keyboard/mouse backend: reboxed or legacy"},
                      input_backend_));
  flag(cli::make_flag(cli::Name{"key-repeat"},
                      cli::Description{"Host key repeat policy: ignore or forward"},
                      key_repeat_));
  flag(cli::make_flag(cli::Name{"no-touch-emulation"},
                      cli::Description{"Disable touch emulation applied on mouse inputs"},
                      no_touch_emulation_));
  flag(cli::make_flag(cli::Name{"server-side-decoration"},
                      cli::Description{"Prefer to use server-side decoration instead of client-side decoration"},
                      server_side_decoration_));
  flag(cli::make_flag(cli::Name{"disabled-sensors"},
                      cli::Description{"Sensors to disable, comma delimited"},
                      disabled_sensors_));
  flag(cli::make_flag(cli::Name{"rootless"},
                      cli::Description{"Run in rootless window mode"},
                      rootless_));

  action([this](const cli::Command::Context &) {
    auto trap = core::posix::trap_signals_for_process(
        {core::posix::Signal::sig_term, core::posix::Signal::sig_int});
    trap->signal_raised().connect([trap](const core::posix::Signal &signal) {
      INFO("Signal %i received. Good night.", static_cast<int>(signal));
      trap->stop();
    });

    if (standalone_ && !experimental_) {
      ERROR("Experimental features selected, but --experimental flag not set");
      return EXIT_FAILURE;
    }

    if (orientation_ != "auto" && orientation_ != "portrait" &&
        orientation_ != "landscape") {
      ERROR("Invalid orientation '%s' (expected auto, portrait, or landscape)",
            orientation_.c_str());
      return EXIT_FAILURE;
    }
    if (renderer_backend_ != "reboxed" && renderer_backend_ != "legacy") {
      ERROR("Invalid renderer '%s' (expected reboxed or legacy)",
            renderer_backend_.c_str());
      return EXIT_FAILURE;
    }
    if (graphics_api_ != "auto" && graphics_api_ != "vulkan" &&
        graphics_api_ != "gles") {
      ERROR("Invalid graphics API '%s' (expected auto, vulkan, or gles)",
            graphics_api_.c_str());
      return EXIT_FAILURE;
    }
    if (renderer_backend_ == "legacy" && graphics_api_ == "vulkan") {
      ERROR("The Vulkan gfxstream transport requires --renderer=reboxed; use --graphics-api=gles for the legacy renderer");
      return EXIT_FAILURE;
    }
    if (input_backend_ != "reboxed" && input_backend_ != "legacy") {
      ERROR("Invalid input backend '%s' (expected reboxed or legacy)",
            input_backend_.c_str());
      return EXIT_FAILURE;
    }
    if (key_repeat_ != "ignore" && key_repeat_ != "forward") {
      ERROR("Invalid key repeat policy '%s' (expected ignore or forward)",
            key_repeat_.c_str());
      return EXIT_FAILURE;
    }
    const bool explicit_window_size = window_size_ != graphics::Rect::Invalid;
    if (explicit_window_size &&
        (window_size_.width() < 320 || window_size_.height() < 320)) {
      ERROR("Window dimensions must each be at least 320 pixels");
      return EXIT_FAILURE;
    }
    if (android_size_.width() < 320 || android_size_.height() < 320) {
      ERROR("Android framebuffer dimensions must each be at least 320 pixels");
      return EXIT_FAILURE;
    }
    if (density_ < 120 || density_ > 640) {
      ERROR("Android density must be between 120 and 640 DPI");
      return EXIT_FAILURE;
    }
    if ((orientation_ == "portrait" && android_size_.width() > android_size_.height()) ||
        (orientation_ == "landscape" && android_size_.width() < android_size_.height())) {
      android_size_ = graphics::Rect{android_size_.height(), android_size_.width()};
    }
    graphics::set_density(density_);
    graphics::emugl::DisplayInfo::get()->set_density(density_);


    if (!fs::exists("/dev/binder") &&
        !common::BinderDeviceAllocator::is_supported()) {
      ERROR("Failed to start because no Binder device can be allocated");
      return EXIT_FAILURE;
    }

    prepare_runtime_directory(SystemConfiguration::instance().socket_dir(),
                              true);
    prepare_runtime_directory(SystemConfiguration::instance().input_device_dir());

    auto rt = Runtime::create();
    auto dispatcher = anbox::common::create_dispatcher_for_runtime(rt);

    if (!standalone_) {
      container_ = std::make_shared<container::Client>(rt);
      container_->register_terminate_handler([trap]() {
        WARNING("Lost connection to container manager, terminating.");
        trap->stop();
      });
    }

    auto input_manager = std::make_shared<input::Manager>(rt);
    auto android_api_stub = std::make_shared<bridge::AndroidApiStub>();

    auto display_frame = graphics::Rect::Invalid;
    if (single_window_)
      display_frame = android_size_;

    const auto should_enable_touch_emulation = utils::get_env_value("ANBOX_ENABLE_TOUCH_EMULATION", "true");
    if (should_enable_touch_emulation == "false" || no_touch_emulation_)
      no_touch_emulation_ = true;

    const auto should_force_server_side_decoration = utils::get_env_value("ANBOX_FORCE_SERVER_SIDE_DECORATION", "false");
    if (should_force_server_side_decoration == "true")
      server_side_decoration_ = true;
    // Borderless is the normal single-window presentation. Window controls
    // are a separate policy: the historical controls were invisible and, at
    // 320 DPI, covered the top-right 84x84 host pixels. Window gates those
    // controls behind the explicit legacy compatibility override rather than
    // requiring compositor decorations to make the Android content safe.
    if (single_window_ && !server_side_decoration_)
      INFO("Single-window lifecycle: borderless host window with legacy overlay controls disabled");

    platform::Configuration platform_config;
    platform_config.single_window = single_window_;
    platform_config.no_touch_emulation = no_touch_emulation_;
    platform_config.display_frame = display_frame;
    platform_config.server_side_decoration = server_side_decoration_;
    platform_config.rootless = rootless_;
    platform_config.legacy_input = input_backend_ == "legacy";
    platform_config.forward_host_key_repeat = key_repeat_ == "forward";

    auto platform = platform::create(utils::get_env_value("ANBOX_PLATFORM", "sdl"),
                                     input_manager,
                                     platform_config);
    if (!platform)
      return EXIT_FAILURE;
    if (!input_manager->create_uinput_devices()) {
      ERROR("Failed to create kernel uinput devices for Android 15 input");
      return EXIT_FAILURE;
    }

    auto app_db = std::make_shared<application::Database>();

    std::shared_ptr<wm::Manager> window_manager;
    bool using_single_window = false;
    if (platform->supports_multi_window() && !single_window_)
      window_manager = std::make_shared<wm::MultiWindowManager>(platform, android_api_stub, app_db);
    else {
      window_manager = std::make_shared<wm::SingleWindowManager>(platform, window_size_, app_db);
      using_single_window = true;
    }

    const auto should_force_software_rendering = utils::get_env_value("ANBOX_FORCE_SOFTWARE_RENDERING", "false");
    auto gl_driver = graphics::GLRendererServer::Config::Driver::Host;
    if (should_force_software_rendering == "true" || use_software_rendering_)
      gl_driver = graphics::GLRendererServer::Config::Driver::Software;

    const bool use_gfxstream = renderer_backend_ == "reboxed" &&
        graphics_api_ != "gles" && graphics::GfxstreamBackend::compiled_in();
    if (graphics_api_ == "vulkan" && !use_gfxstream) {
      ERROR("Vulkan gfxstream was requested but this build has no gfxstream backend");
      return EXIT_FAILURE;
    }
    if (graphics_api_ == "auto" && renderer_backend_ == "reboxed" &&
        !use_gfxstream)
      WARNING("Modern gfxstream is unavailable; falling back to the legacy GLES2 compatibility transport");

    graphics::GLRendererServer::Config renderer_config{
        gl_driver,
        single_window_,
        renderer_backend_ == "legacy"
            ? graphics::GLRendererServer::Config::Backend::Legacy
            : graphics::GLRendererServer::Config::Backend::Reboxed,
        allow_software_fallback_,
        use_gfxstream,
        use_gfxstream,
        android_size_.width(),
        android_size_.height()};
    auto gl_server = std::make_shared<graphics::GLRendererServer>(renderer_config, window_manager);

    platform->set_window_manager(window_manager);
    platform->set_renderer(gl_server->renderer());
    window_manager->setup();

    auto app_manager = std::static_pointer_cast<application::Manager>(android_api_stub);
    if (!using_single_window) {
      // When we're not running single window mode we need to restrict ourself to
      // only launch applications in freeform mode as otherwise the window tracking
      // doesn't work.
      app_manager = std::make_shared<application::RestrictedManager>(
          android_api_stub, wm::Stack::Id::Freeform);
    }

    auto audio_server = std::make_shared<audio::Server>(rt, platform);

    const auto socket_path = SystemConfiguration::instance().socket_dir();

    auto sensors_state = std::make_shared<application::SensorsState>();
    std::stringstream disabled_sensors_stream(disabled_sensors_);
    std::string disabled_sensor_name;
    while (std::getline(disabled_sensors_stream, disabled_sensor_name, ',')) {
      sensors_state->disabled_sensors |= application::SensorTypeHelper::FromString(disabled_sensor_name);
    }
    auto gps_info_broker = std::make_shared<application::GpsInfoBroker>();

    // The qemu pipe is used as a very fast communication channel between guest
    // and host for things like the GLES emulation/translation, the RIL or ADB.
    auto qemu_pipe_connector =
        std::make_shared<network::PublishedSocketConnector>(
            utils::string_format("%s/qemu_pipe", socket_path), rt,
            std::make_shared<qemu::PipeConnectionCreator>(
                gl_server->renderer(), gl_server->gfxstream(), rt,
                sensors_state, gps_info_broker));
    ::chmod(qemu_pipe_connector->socket_file().c_str(), S_IRWXU | S_IRWXG | S_IRWXO);

    boost::asio::steady_timer appmgr_start_timer(rt->service());

    auto bridge_connector = std::make_shared<network::PublishedSocketConnector>(
        utils::string_format("%s/anbox_bridge", socket_path), rt,
        std::make_shared<rpc::ConnectionCreator>(
            [&](const std::shared_ptr<network::MessageSender> &sender) {
              auto pending_calls = std::make_shared<rpc::PendingCallCache>();
              auto rpc_channel =
                  std::make_shared<rpc::Channel>(pending_calls, sender);
              // This is safe as long as we only support a single client. If we
              // support
              // more than one one day we need proper dispatching to the right
              // one.
              android_api_stub->set_rpc_channel(rpc_channel);

              auto server = std::make_shared<bridge::PlatformApiSkeleton>(
                  pending_calls, platform, window_manager, app_db);
              server->register_boot_finished_handler([&]() {
                DEBUG("Android successfully booted");
                android_api_stub->ready().set(true);
                appmgr_start_timer.expires_after(default_appmgr_startup_delay);
                appmgr_start_timer.async_wait([&](const boost::system::error_code &err) {
                  if (err)
                    return;
                  launch_appmgr_if_needed(android_api_stub);
                });
              });
              return std::make_shared<bridge::PlatformMessageProcessor>(
                  sender, server, pending_calls);
            }));
    ::chmod(bridge_connector->socket_file().c_str(), S_IRWXU | S_IRWXG | S_IRWXO);

    container::Configuration container_configuration;

    // Instruct healthd to fake battery level as it may take it from other connected
    // devices like mouse or keyboard and will incorrectly show a system popup to
    // shutdown the Android system because of low battery. This prevents any kind of
    // input as focus is bound to the system popup exclusively.
    //
    // See https://github.com/anbox/anbox/issues/780 for further details.
    container_configuration.extra_properties.push_back("ro.boot.fake_battery=1");

    // Keep the x86_64 GSI on a 64-bit primary zygote. Android 15 Connectivity
    // ships native service libraries such as libservice-connectivity.so only in
    // the 64-bit APEX libdir, so forcing zygote32 makes system_server abort.
    container_configuration.extra_properties.push_back("ro.zygote=zygote64_32");

    // The GSI expects its vendor to declare the logical display density and
    // GLES feature level. Keep density controlled by the session option and
    // describe the actual legacy decoder capability truthfully. Leaving the
    // GLES property absent reports reqGlEsVersion=0x0 to applications and
    // makes Chromium WebView reject even a GLES2 context.
    container_configuration.extra_properties.push_back(
        utils::string_format("ro.sf.lcd_density=%d", density_));
    container_configuration.extra_properties.push_back(
        use_gfxstream ? "ro.opengles.version=196608"
                      : "ro.opengles.version=131072");
    container_configuration.extra_properties.push_back(
        use_gfxstream ? "ro.anbox.graphics_transport=gfxstream"
                      : "ro.anbox.graphics_transport=compatibility");

    // The container exposes neither a virtio-gpu DRM render node nor the
    // goldfish_sync kernel device. gfxstream therefore completes guest work
    // synchronously with glFinish and does not advertise native-fence EGL
    // extensions. Tell SurfaceFlinger the same truth; otherwise Android 15
    // waits for sync-file fences that can never exist, leaving SurfaceView
    // buffers (notably Firefox/WebView content) outside the composition list.
    container_configuration.extra_properties.push_back(
        "ro.surface_flinger.running_without_sync_framework=true");

    if (server_side_decoration_)
      container_configuration.extra_properties.push_back("ro.anbox.no_decorations=1");

    if (!standalone_) {
      container_configuration.bind_mounts = {
          {socket_path, "/dev/anbox_sockets"},
      };

      container_configuration.devices = {
          {"/dev/fuse", {0666}},
      };
      for (const auto &event_path : input_manager->uinput_event_paths())
        // The container-management RPC carries the device path and mode but
        // not DeviceSpecification::old_device_name. Keep the real event path
        // as the guest path so its major/minor survives RPC serialization.
        container_configuration.devices.emplace(event_path,
                                                 container::DeviceSpecification{0666});

      // The container client connects to the manager over an asynchronous
      // local socket. Schedule its first RPC after the runtime begins so a
      // minimal/no-DBus desktop session cannot race the connection setup.
      // Keep the timer alive until its callback runs. A stack-allocated timer
      // is destroyed when this setup block ends, which cancels its wait and
      // leaves a live SDL window backed by a container that was never started.
      auto container_start_timer =
          std::make_shared<boost::asio::steady_timer>(rt->service());
      container_start_timer->expires_after(std::chrono::milliseconds{100});
      container_start_timer->async_wait(
          [this, configuration = container_configuration, container_start_timer](
              const boost::system::error_code &error) {
            if (!error)
              container_->start(configuration);
          });
    }

    std::unique_ptr<sdbus::IConnection> connection;
    std::unique_ptr<ApplicationManagerServer> app_manager_server;
    std::unique_ptr<SensorsServer> sensors_server;
    std::unique_ptr<GpsServer> gps_server;
    if (!no_dbus_service_) {
      connection = use_system_dbus_
                       ? sdbus::createSystemBusConnection(dbus::interface::Service::name())
                       : sdbus::createSessionBusConnection(dbus::interface::Service::name());
      app_manager_server = std::make_unique<ApplicationManagerServer>(
          *connection, dbus::interface::Service::path(), app_manager);
      sensors_server = std::make_unique<SensorsServer>(
          *connection, dbus::interface::Service::path(), sensors_state);
      gps_server = std::make_unique<GpsServer>(
          *connection, dbus::interface::Service::path(), gps_info_broker);
      connection->enterEventLoopAsync();
    }

    rt->start();
    trap->run();

    if (!standalone_) {
      // Stop the container which should close all open connections we have on
      // our side and should terminate all services.
      container_->stop();
    }

    rt->stop();

    return EXIT_SUCCESS;
  });
}
