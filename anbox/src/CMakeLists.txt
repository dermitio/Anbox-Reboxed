include_directories(
  ${Boost_INCLUDE_DIRS}
  ${MIRCLIENT_INCLUDE_DIRS}
  ${DBUS_INCLUDE_DIRS}
  ${SDL2_INCLUDE_DIRS}
  ${SDL2_IMAGE_INCLUDE_DIRS}
  ${LXC_INCLUDE_DIRS}
  ${MIRCLIENT_INCLUDE_DIRS}
  ${LIBSYSTEMD_INCLUDE_DIRS}
  ${GFXSTREAM_INCLUDE_DIRS}
  ${CMAKE_CURRENT_BINARY_DIR}
  ${CMAKE_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/external
  ${CMAKE_SOURCE_DIR}/external/process-cpp-minimal/include
  ${CMAKE_SOURCE_DIR}/external/android-emugl/shared
  ${CMAKE_SOURCE_DIR}/external/android-emugl/host/include
  ${CMAKE_BINARY_DIR}/external/android-emugl/host/include
  ${CMAKE_SOURCE_DIR}/external/android-emugl/shared/OpenglCodecCommon
  ${CMAKE_SOURCE_DIR}/external/android-emugl/host/libs
  ${CMAKE_SOURCE_DIR}/external/android-emugl/host/include/libOpenglRender
  ${CMAKE_SOURCE_DIR}/external/android-emugl/host/libs/GLESv1_dec
  ${CMAKE_BINARY_DIR}/external/android-emugl/host/libs/GLESv1_dec
  ${CMAKE_SOURCE_DIR}/external/android-emugl/host/libs/GLESv2_dec
  ${CMAKE_BINARY_DIR}/external/android-emugl/host/libs/GLESv2_dec
  ${CMAKE_SOURCE_DIR}/external/android-emugl/host/libs/renderControl_dec
  ${CMAKE_BINARY_DIR}/external/android-emugl/host/libs/renderControl_dec
  ${CMAKE_SOURCE_DIR}/external/cpu_features/include
  ${CMAKE_SOURCE_DIR}/external/sdbus-cpp/include
  ${GFXSTREAM_INTERFACE_INCLUDE}
)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DBINDERFS_PATH=\"\\\"${BINDERFS_PATH}\\\"\"")

protobuf_generate_cpp(
    GENERATED_PROTOBUF_RPC_SRCS GENERATED_PROTOBUF_RPC_HDRS
    anbox/protobuf/anbox_rpc.proto)

protobuf_generate_cpp(
    GENERATED_PROTOBUF_BRIDGE_SRCS GENERATED_PROTOBUF_BRIDGE_HDRS
    anbox/protobuf/anbox_bridge.proto)

protobuf_generate_cpp(
    GENERATED_PROTOBUF_CONTAINER_SRCS GENERATED_PROTOBUF_CONTAINER_HDRS
    anbox/protobuf/anbox_container.proto)


add_library(anbox-protobuf
    STATIC
    ${GENERATED_PROTOBUF_BRIDGE_SRCS}
    ${GENERATED_PROTOBUF_BRIDGE_HDRS}
    ${GENERATED_PROTOBUF_RPC_SRCS}
    ${GENERATED_PROTOBUF_RPC_HDRS}
    ${GENERATED_PROTOBUF_CONTAINER_SRCS}
    ${GENERATED_PROTOBUF_CONTAINER_HDRS}
    anbox/protobuf/anbox_rpc.proto
    anbox/protobuf/anbox_bridge.proto
    anbox/protobuf/anbox_container.proto
    anbox/protobuf/google_protobuf_guard.cpp)
target_link_libraries(anbox-protobuf
    ${PROTOBUF_LITE_LIBRARIES})

set(XML2CPP ${CMAKE_BINARY_DIR}/external/sdbus-cpp/src/sdbus-cpp-build/tools/sdbus-c++-xml2cpp)

macro(DBusServer BaseName)
    add_custom_command(
        COMMAND ${XML2CPP} --adaptor=${CMAKE_CURRENT_BINARY_DIR}/${BaseName}_server_glue.h ${CMAKE_CURRENT_SOURCE_DIR}/anbox/dbus/${BaseName}.xml
        DEPENDS anbox/dbus/${BaseName}.xml
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${BaseName}_server_glue.h
        COMMENT "Generating server code for DBUS (${BaseName})"
    )
    list(APPEND DBUS_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/${BaseName}_server_glue.h")
endmacro()

macro(DBusClient BaseName)
    add_custom_command(
        COMMAND ${XML2CPP} --proxy=${CMAKE_CURRENT_BINARY_DIR}/${BaseName}_client_glue.h ${CMAKE_CURRENT_SOURCE_DIR}/anbox/dbus/${BaseName}.xml
        DEPENDS anbox/dbus/${BaseName}.xml
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${BaseName}_client_glue.h
        COMMENT "Generating client code for DBUS (${BaseName})"
    )
    list(APPEND DBUS_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/${BaseName}_client_glue.h")
endmacro()

set(DBUS_GENERATED)

DBusServer(application_manager)
DBusClient(application_manager)
DBusServer(sensors)
DBusServer(gps)

set(SOURCES
    anbox/android/intent.cpp
    anbox/android/intent.h
    anbox/android/ip_config_builder.cpp
    anbox/android/ip_config_builder.h

    anbox/application/database.cpp
    anbox/application/database.h
    anbox/application/launcher_storage.cpp
    anbox/application/launcher_storage.h
    anbox/application/manager.h
    anbox/application/sensor_type.cpp
    anbox/application/sensor_type.h

    anbox/audio/client_info.h
    anbox/audio/server.cpp
    anbox/audio/server.h
    anbox/audio/sink.h
    anbox/audio/source.h

    anbox/bridge/android_api_stub.cpp
    anbox/bridge/android_api_stub.h
    anbox/bridge/platform_api_skeleton.cpp
    anbox/bridge/platform_api_skeleton.h
    anbox/bridge/platform_message_processor.cpp
    anbox/bridge/platform_message_processor.h

    anbox/build/config.h
    anbox/build/config.h.in

    anbox/cmds/container_manager.cpp
    anbox/cmds/container_manager.h
    anbox/cmds/launch.cpp
    anbox/cmds/memory.cpp
    anbox/cmds/launch.h
    anbox/cmds/android_tool.cpp
    anbox/cmds/android_tool.h
    anbox/cmds/session_manager.cpp
    anbox/cmds/session_manager.h
    anbox/cmds/system_info.cpp
    anbox/cmds/system_info.h
    anbox/cmds/version.cpp
    anbox/cmds/version.h
    anbox/cmds/wait_ready.cpp
    anbox/cmds/wait_ready.h
    anbox/cmds/check_features.cpp
    anbox/cmds/check_features.h
    anbox/cmds/compatibility.cpp
    anbox/cmds/compatibility.h
    anbox/cmds/display_status.cpp
    anbox/cmds/display_status.h
    anbox/cmds/reboxed_tools.cpp
    anbox/cmds/reboxed_tools.h

    anbox/common/binary_writer.cpp
    anbox/common/binary_writer.h
    anbox/common/dispatcher.cpp
    anbox/common/dispatcher.h
    anbox/common/fd.cpp
    anbox/common/fd.h
    anbox/common/fd_sets.h
    anbox/common/loop_device_allocator.cpp
    anbox/common/loop_device_allocator.h
    anbox/common/loop_device.cpp
    anbox/common/loop_device.h
    anbox/common/message_channel.cpp
    anbox/common/message_channel.h
    anbox/common/mount_entry.cpp
    anbox/common/mount_entry.h
    anbox/common/scope_ptr.h
    anbox/common/small_vector.h
    anbox/common/type_traits.h
    anbox/common/variable_length_array.h
    anbox/common/wait_handle.cpp
    anbox/common/wait_handle.h
    anbox/common/binderfs.h
    anbox/common/binder_device.cpp
    anbox/common/binder_device.h
    anbox/common/binder_device_allocator.cpp
    anbox/common/binder_device_allocator.h

    anbox/container/client.cpp
    anbox/container/client.h
    anbox/container/configuration.h
    anbox/container/container.cpp
    anbox/container/container.h
    anbox/container/lxc_container.cpp
    anbox/container/lxc_container.h
    anbox/container/management_api_message_processor.cpp
    anbox/container/management_api_message_processor.h
    anbox/container/management_api_skeleton.cpp
    anbox/container/management_api_skeleton.h
    anbox/container/management_api_stub.cpp
    anbox/container/management_api_stub.h
    anbox/container/service.cpp
    anbox/container/service.h

    anbox/dbus/bus.cpp
    anbox/dbus/bus.h
    anbox/dbus/interface.h
    ${DBUS_GENERATED}
    anbox/dbus/application_manager_server.cpp
    anbox/dbus/application_manager_server.h
    anbox/dbus/application_manager_client.cpp
    anbox/dbus/application_manager_client.h
    anbox/dbus/sensors_server.cpp
    anbox/dbus/sensors_server.h
    anbox/dbus/gps_server.cpp
    anbox/dbus/gps_server.h

    anbox/graphics/buffered_io_stream.cpp
    anbox/graphics/buffered_io_stream.h
    anbox/graphics/buffer_queue.cpp
    anbox/graphics/buffer_queue.h
    anbox/graphics/density.cpp
    anbox/graphics/density.h
    anbox/graphics/coordinate_transform.cpp
    anbox/graphics/coordinate_transform.h
    anbox/graphics/rendered_viewport.cpp
    anbox/graphics/rendered_viewport.h
    anbox/graphics/gl_extensions.h
    anbox/graphics/gl_renderer_server.cpp
    anbox/graphics/gl_renderer_server.h
    anbox/graphics/gfxstream_backend.cpp
    anbox/graphics/gfxstream_backend.h
    anbox/graphics/gfxstream_message_processor.cpp
    anbox/graphics/gfxstream_message_processor.h
    anbox/graphics/layer_composer.cpp
    anbox/graphics/layer_composer.h
    anbox/graphics/multi_window_composer_strategy.cpp
    anbox/graphics/multi_window_composer_strategy.h
    anbox/graphics/opengles_message_processor.cpp
    anbox/graphics/opengles_message_processor.h
    anbox/graphics/opengles_socket_connection.cpp
    anbox/graphics/opengles_socket_connection.h
    anbox/graphics/primitives.h
    anbox/graphics/program_family.cpp
    anbox/graphics/program_family.h
    anbox/graphics/rect.cpp
    anbox/graphics/rect.h
    anbox/graphics/renderer.h
    anbox/graphics/single_window_composer_strategy.cpp
    anbox/graphics/single_window_composer_strategy.h

    anbox/graphics/emugl/ColorBuffer.cpp
    anbox/graphics/emugl/ColorBuffer.h
    anbox/graphics/emugl/DispatchTables.h
    anbox/graphics/emugl/DisplayManager.cpp
    anbox/graphics/emugl/DisplayManager.h
    anbox/graphics/emugl/ReadBuffer.cpp
    anbox/graphics/emugl/ReadBuffer.h
    anbox/graphics/emugl/Renderable.cpp
    anbox/graphics/emugl/Renderable.h
    anbox/graphics/emugl/RenderApi.cpp
    anbox/graphics/emugl/RenderApi.h
    anbox/graphics/emugl/RenderContext.cpp
    anbox/graphics/emugl/RenderContext.h
    anbox/graphics/emugl/RenderControl.cpp
    anbox/graphics/emugl/RenderControl.h
    anbox/graphics/emugl/RendererConfig.cpp
    anbox/graphics/emugl/RendererConfig.h
    anbox/graphics/emugl/Renderer.cpp
    anbox/graphics/emugl/Renderer.h
    anbox/graphics/emugl/RenderThread.cpp
    anbox/graphics/emugl/RenderThread.h
    anbox/graphics/emugl/RenderThreadInfo.cpp
    anbox/graphics/emugl/RenderThreadInfo.h
    anbox/graphics/emugl/TextureDraw.cpp
    anbox/graphics/emugl/TextureDraw.h
    anbox/graphics/emugl/TextureResize.cpp
    anbox/graphics/emugl/TextureResize.h
    anbox/graphics/emugl/TimeUtils.cpp
    anbox/graphics/emugl/TimeUtils.h
    anbox/graphics/emugl/WindowSurface.cpp
    anbox/graphics/emugl/WindowSurface.h

    anbox/input/device.cpp
    anbox/input/device.h
    anbox/input/event_sink.h
    anbox/input/dispatcher.cpp
    anbox/input/dispatcher.h
    anbox/input/keyboard.cpp
    anbox/input/keyboard.h
    anbox/input/pointer.cpp
    anbox/input/pointer.h
    anbox/input/manager.cpp
    anbox/input/manager.h

    anbox/network/base_socket_messenger.cpp
    anbox/network/base_socket_messenger.h
    anbox/network/connection_context.cpp
    anbox/network/connection_context.h
    anbox/network/connection_creator.cpp
    anbox/network/connection_creator.h
    anbox/network/connections.h
    anbox/network/connector.h
    anbox/network/credentials.cpp
    anbox/network/credentials.h
    anbox/network/delegate_connection_creator.h
    anbox/network/delegate_message_processor.cpp
    anbox/network/delegate_message_processor.h
    anbox/network/fd_socket_transmission.cpp
    anbox/network/fd_socket_transmission.h
    anbox/network/local_socket_messenger.cpp
    anbox/network/local_socket_messenger.h
    anbox/network/message_processor.h
    anbox/network/message_receiver.h
    anbox/network/message_sender.h
    anbox/network/published_socket_connector.cpp
    anbox/network/published_socket_connector.h
    anbox/network/socket_connection.cpp
    anbox/network/socket_connection.h
    anbox/network/socket_helper.cpp
    anbox/network/socket_helper.h
    anbox/network/socket_messenger.cpp
    anbox/network/socket_messenger.h
    anbox/network/tcp_socket_connector.cpp
    anbox/network/tcp_socket_connector.h
    anbox/network/tcp_socket_messenger.cpp
    anbox/network/tcp_socket_messenger.h

    anbox/platform/base_platform.cpp
    anbox/platform/base_platform.h
    anbox/platform/null/platform.cpp
    anbox/platform/null/platform.h
    anbox/platform/sdl/audio_sink.cpp
    anbox/platform/sdl/audio_sink.h
    anbox/platform/sdl/keycode_converter.cpp
    anbox/platform/sdl/keycode_converter.h
    anbox/platform/sdl/platform.cpp
    anbox/platform/sdl/platform.h
    anbox/platform/sdl/sdl_wrapper.h
    anbox/platform/sdl/window.cpp
    anbox/platform/sdl/window.h
    anbox/platform/sdl/window_geometry.cpp
    anbox/platform/sdl/window_geometry.h

    anbox/protobuf/anbox_bridge.proto
    anbox/protobuf/anbox_container.proto
    anbox/protobuf/anbox_rpc.proto
    anbox/protobuf/google_protobuf_guard.cpp

    anbox/qemu/adb_message_processor.cpp
    anbox/qemu/adb_message_processor.h
    anbox/qemu/at_parser.cpp
    anbox/qemu/at_parser.h
    anbox/qemu/bootanimation_message_processor.cpp
    anbox/qemu/bootanimation_message_processor.h
    anbox/qemu/boot_properties_message_processor.cpp
    anbox/qemu/boot_properties_message_processor.h
    anbox/qemu/camera_message_processor.cpp
    anbox/qemu/camera_message_processor.h
    anbox/qemu/fingerprint_message_processor.cpp
    anbox/qemu/fingerprint_message_processor.h
    anbox/qemu/gps_message_processor.cpp
    anbox/qemu/gps_message_processor.h
    anbox/qemu/gsm_message_processor.cpp
    anbox/qemu/gsm_message_processor.h
    anbox/qemu/hwcontrol_message_processor.cpp
    anbox/qemu/hwcontrol_message_processor.h
    anbox/qemu/null_message_processor.cpp
    anbox/qemu/null_message_processor.h
    anbox/qemu/pipe_connection_creator.cpp
    anbox/qemu/pipe_connection_creator.h
    anbox/qemu/refcount_message_processor.cpp
    anbox/qemu/refcount_message_processor.h
    anbox/qemu/qemud_message_processor.cpp
    anbox/qemu/qemud_message_processor.h
    anbox/qemu/sensors_message_processor.cpp
    anbox/qemu/sensors_message_processor.h

    anbox/rpc/channel.cpp
    anbox/rpc/channel.h
    anbox/rpc/connection_creator.cpp
    anbox/rpc/connection_creator.h
    anbox/rpc/constants.h
    anbox/rpc/make_protobuf_object.h
    anbox/rpc/message_processor.cpp
    anbox/rpc/message_processor.h
    anbox/rpc/pending_call_cache.cpp
    anbox/rpc/pending_call_cache.h
    anbox/rpc/template_message_processor.h

    anbox/testing/gtest_utils.h

    anbox/ui/splash_screen.cpp
    anbox/ui/splash_screen.h

    anbox/utils/environment_file.cpp
    anbox/utils/environment_file.h

    anbox/wm/display.cpp
    anbox/wm/display.h
    anbox/wm/manager.cpp
    anbox/wm/manager.h
    anbox/wm/multi_window_manager.cpp
    anbox/wm/multi_window_manager.h
    anbox/wm/single_window_manager.cpp
    anbox/wm/single_window_manager.h
    anbox/wm/stack.cpp
    anbox/wm/stack.h
    anbox/wm/task.cpp
    anbox/wm/task.h
    anbox/wm/window.cpp
    anbox/wm/window.h
    anbox/wm/window_state.cpp
    anbox/wm/window_state.h

    anbox/cli.cpp
    anbox/cli.h
    anbox/daemon.cpp
    anbox/daemon.h
    anbox/defer_action.h
    anbox/do_not_copy_or_move.h
    anbox/logger.cpp
    anbox/memory_manager.cpp
    anbox/logger.h
    anbox/not_reachable.cpp
    anbox/not_reachable.h
    anbox/optional.h
    anbox/utils.cpp
    anbox/utils.h
    anbox/runtime.cpp
    anbox/runtime.h
    anbox/system_configuration.cpp
    anbox/system_configuration.h)

add_library(anbox-core STATIC ${SOURCES})
add_dependencies(anbox-core sdbus-cpp)
target_link_libraries(anbox-core
  ${Boost_LDFLAGS}
  ${Boost_LIBRARIES}
  ${SDL2_LDFLAGS}
  ${SDL2_LIBRARIES}
  ${SDL2_IMAGE_LDFLAGS}
  ${SDL2_IMAGE_LIBRARIES}
  $<$<BOOL:${ENABLE_X11}>:X11::X11>
  ${LXC_LDFLAGS}
  ${LXC_LIBRARIES}
  ${MIRCLIENT_LDFLAGS}
  ${MIRCLIENT_LIBRARIES}
  ${LIBSYSTEMD_LDFLAGS}
  ${LIBSYSTEMD_LIBRARIES}
  cpu_features
  pthread
  process-cpp
  emugl_common
  GLESv1_dec
  GLESv2_dec
  renderControl_dec
  OpenGLESDispatch
  OpenglCodecCommon
  anbox-protobuf
  sdbus-c++
  xdg
  # Keep the modern backend after the legacy decoder archives.  Both code
  # bases contain generated classes with historical, un-namespaced symbols
  # (for example gles2_server_context_t::initDispatchByName) but incompatible
  # layouts.  If gfxstream is seen first, the linker leaves the legacy
  # reference undefined and binds it to gfxstream at runtime, which writes a
  # modern dispatch table past the legacy RenderThreadInfo object.
  ${GFXSTREAM_LDFLAGS}
  ${GFXSTREAM_LIBRARIES})

link_directories(${CMAKE_CURRENT_BINARY_DIR}/../external/sdbus-cpp/src/sdbus-cpp-build/)

add_executable(anbox-reboxed main.cpp ${BACKWARD_ENABLE})
target_link_libraries(anbox-reboxed
    anbox-core)
# Do not export generated legacy-emugl symbols to shared libraries.  In
# particular this prevents the inverse interposition, where gfxstream would
# bind its internal decoder calls to the now-linked legacy implementation.
target_link_options(anbox-reboxed PRIVATE "LINKER:--exclude-libs,ALL")
if (GFXSTREAM_BACKEND_LIBRARY)
  get_filename_component(GFXSTREAM_BACKEND_LIBRARY_DIR
      "${GFXSTREAM_BACKEND_LIBRARY}" DIRECTORY)
  set_target_properties(anbox-reboxed PROPERTIES
      BUILD_RPATH "${GFXSTREAM_BACKEND_LIBRARY_DIR}"
      INSTALL_RPATH "\$ORIGIN/../lib/anbox-reboxed")
endif()
add_backward(anbox-reboxed)
# Keep the historical build-tree command available as a compatibility alias.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/../scripts/anbox-compat
               ${CMAKE_CURRENT_BINARY_DIR}/anbox COPYONLY)

set(ASHMEM_IOCTL_COMPAT_SOURCE
    ${CMAKE_CURRENT_SOURCE_DIR}/anbox/container/ashmem_ioctl_compat.c)
set(ASHMEM_IOCTL_COMPAT64
    ${CMAKE_CURRENT_BINARY_DIR}/libanbox_ashmem_ioctl_compat.so)
set(ASHMEM_IOCTL_COMPAT32
    ${CMAKE_CURRENT_BINARY_DIR}/libanbox_ashmem_ioctl_compat32.so)
add_custom_command(
    OUTPUT ${ASHMEM_IOCTL_COMPAT64}
    COMMAND ${CMAKE_C_COMPILER} -shared -fPIC -fno-stack-protector -nostdlib
            -Wl,-soname,libanbox_ashmem_ioctl_compat.so
            ${ASHMEM_IOCTL_COMPAT_SOURCE} -o ${ASHMEM_IOCTL_COMPAT64}
    DEPENDS ${ASHMEM_IOCTL_COMPAT_SOURCE})
add_custom_command(
    OUTPUT ${ASHMEM_IOCTL_COMPAT32}
    COMMAND ${CMAKE_C_COMPILER} -m32 -shared -fPIC -fno-stack-protector -nostdlib
            -Wl,-soname,libanbox_ashmem_ioctl_compat.so
            ${ASHMEM_IOCTL_COMPAT_SOURCE} -o ${ASHMEM_IOCTL_COMPAT32}
    DEPENDS ${ASHMEM_IOCTL_COMPAT_SOURCE})
add_custom_target(anbox-ashmem-ioctl-compat
    DEPENDS ${ASHMEM_IOCTL_COMPAT64} ${ASHMEM_IOCTL_COMPAT32})
add_dependencies(anbox-reboxed anbox-ashmem-ioctl-compat)

# The API 35 vendor pipe shim must follow descriptor lifetime correctly.  Build
# a container-side replacement from the versioned GSI-vendor source so it can
# be updated without rebuilding or rewriting vendor.img.
set(PIPE_COMPAT_SOURCE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../gsi-vendor/pipe-compat/anbox_pipe_compat.c)
set(PIPE_COMPAT_MAP
    ${CMAKE_CURRENT_SOURCE_DIR}/../../gsi-vendor/pipe-compat/anbox_pipe_compat.map)
set(PIPE_COMPAT64
    ${CMAKE_CURRENT_BINARY_DIR}/libanbox_pipe_compat.so)
set(PIPE_COMPAT32
    ${CMAKE_CURRENT_BINARY_DIR}/libanbox_pipe_compat32.so)
add_custom_command(
    OUTPUT ${PIPE_COMPAT64}
    COMMAND ${CMAKE_C_COMPILER} -shared -fPIC -fno-stack-protector -nostdlib
            -Wl,-soname,libanbox_pipe_compat.so
            -Wl,--version-script,${PIPE_COMPAT_MAP}
            ${PIPE_COMPAT_SOURCE} -o ${PIPE_COMPAT64}
    DEPENDS ${PIPE_COMPAT_SOURCE} ${PIPE_COMPAT_MAP})
add_custom_command(
    OUTPUT ${PIPE_COMPAT32}
    COMMAND ${CMAKE_C_COMPILER} -m32 -shared -fPIC -fno-stack-protector -nostdlib
            -Wl,-soname,libanbox_pipe_compat.so
            -Wl,--version-script,${PIPE_COMPAT_MAP}
            ${PIPE_COMPAT_SOURCE} -o ${PIPE_COMPAT32}
    DEPENDS ${PIPE_COMPAT_SOURCE} ${PIPE_COMPAT_MAP})
add_custom_target(anbox-pipe-compat
    DEPENDS ${PIPE_COMPAT64} ${PIPE_COMPAT32})
add_dependencies(anbox-reboxed anbox-pipe-compat)

set(INSTALLD_SELINUX_COMPAT_SOURCE
    ${CMAKE_CURRENT_SOURCE_DIR}/anbox/container/installd_selinux_compat.c)
set(INSTALLD_SELINUX_COMPAT
    ${CMAKE_CURRENT_BINARY_DIR}/libanbox_installd_selinux_compat.so)
add_custom_command(
    OUTPUT ${INSTALLD_SELINUX_COMPAT}
    COMMAND ${CMAKE_C_COMPILER} -shared -fPIC -fno-stack-protector -nostdlib
            -Wl,-soname,libanbox_installd_selinux_compat.so
            ${INSTALLD_SELINUX_COMPAT_SOURCE} -o ${INSTALLD_SELINUX_COMPAT}
    DEPENDS ${INSTALLD_SELINUX_COMPAT_SOURCE})
add_custom_target(anbox-installd-selinux-compat
    DEPENDS ${INSTALLD_SELINUX_COMPAT})
add_dependencies(anbox-reboxed anbox-installd-selinux-compat)

# Android's stock helper cannot prepare user directories when the host has
# SELinux disabled because security.selinux xattrs are necessarily absent.
# Keep this tiny compatibility helper static so it can execute in Android's
# bionic-only root filesystem.
add_executable(vold_prepare_subdirs_anbox
    anbox/container/vold_prepare_subdirs_anbox.c)
set_target_properties(vold_prepare_subdirs_anbox PROPERTIES
    LINK_FLAGS "-static")

install(
  TARGETS anbox-reboxed vold_prepare_subdirs_anbox
  RUNTIME DESTINATION bin
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib/static)
install(PROGRAMS ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/anbox-compat
        DESTINATION bin RENAME anbox)
install(FILES ${ASHMEM_IOCTL_COMPAT64}
        DESTINATION lib/anbox
        RENAME libanbox_ashmem_ioctl_compat.so)
install(FILES ${ASHMEM_IOCTL_COMPAT32}
        DESTINATION lib/anbox
        RENAME libanbox_ashmem_ioctl_compat32.so)
install(FILES ${PIPE_COMPAT64}
        DESTINATION lib/anbox
        RENAME libanbox_pipe_compat.so)
install(FILES ${PIPE_COMPAT32}
        DESTINATION lib/anbox
        RENAME libanbox_pipe_compat32.so)
install(FILES ${INSTALLD_SELINUX_COMPAT}
        DESTINATION lib/anbox
        RENAME libanbox_installd_selinux_compat.so)
install(PROGRAMS ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/anbox-data-image
        DESTINATION lib/anbox)
install(PROGRAMS
        ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/anbox-signature-spoof
        ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/build-signature-spoof-framework
        ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/patch-signature-spoof-smali
        ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/verify-webview-renderer
        DESTINATION lib/anbox)
install(PROGRAMS ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/anbox-gles-compat
        DESTINATION lib/anbox)
install(PROGRAMS ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/anbox-gfxstream-compat
        DESTINATION lib/anbox)
install(PROGRAMS ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/anbox-native-bridge
                 ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/anbox-reboxed-bridge
        DESTINATION lib/anbox)

find_program(ANBOX_CLANG_EXECUTABLE clang REQUIRED)
set(ANBOX_ANDROID_SDK_ROOT "" CACHE PATH
    "Android SDK used to build the Native Bridge JNI probe APK")
if(NOT ANBOX_ANDROID_SDK_ROOT AND DEFINED ENV{ANDROID_SDK_ROOT})
  set(ANBOX_ANDROID_SDK_ROOT "$ENV{ANDROID_SDK_ROOT}")
endif()
if(NOT ANBOX_ANDROID_SDK_ROOT AND DEFINED ENV{ANDROID_HOME})
  set(ANBOX_ANDROID_SDK_ROOT "$ENV{ANDROID_HOME}")
endif()
if(NOT ANBOX_ANDROID_SDK_ROOT AND EXISTS "$ENV{HOME}/Android/Sdk")
  set(ANBOX_ANDROID_SDK_ROOT "$ENV{HOME}/Android/Sdk")
endif()
set(ANBOX_ANDROID_SDK_AVAILABLE OFF)
if(EXISTS "${ANBOX_ANDROID_SDK_ROOT}/platforms" AND
   EXISTS "${ANBOX_ANDROID_SDK_ROOT}/build-tools")
  set(ANBOX_ANDROID_SDK_AVAILABLE ON)
endif()
option(ANBOX_BUILD_ANDROID_PROBES
    "Build Android Native Bridge and signature-spoof probe APKs"
    ${ANBOX_ANDROID_SDK_AVAILABLE})

if(ANBOX_BUILD_ANDROID_PROBES)
  if(NOT ANBOX_ANDROID_SDK_AVAILABLE)
    message(FATAL_ERROR
        "ANBOX_BUILD_ANDROID_PROBES requires an Android SDK with platforms and build-tools; set ANBOX_ANDROID_SDK_ROOT")
  endif()

  set(NATIVE_BRIDGE_TEST_ROOT ${CMAKE_CURRENT_BINARY_DIR}/native-bridge-tests)
  set(NATIVE_BRIDGE_PROBE_APK ${NATIVE_BRIDGE_TEST_ROOT}/ReboxedNativeBridgeProbe.apk)
  file(GLOB_RECURSE NATIVE_BRIDGE_PROBE_SOURCES CONFIGURE_DEPENDS
       ${CMAKE_CURRENT_SOURCE_DIR}/../android/native-bridge-probe/*)
  add_custom_command(
      OUTPUT ${NATIVE_BRIDGE_PROBE_APK}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${NATIVE_BRIDGE_TEST_ROOT}
      COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/build-native-bridge-probe
              ${CMAKE_CURRENT_SOURCE_DIR}/../android/native-bridge-probe
              ${NATIVE_BRIDGE_PROBE_APK}
              ${ANBOX_ANDROID_SDK_ROOT}
              ${ANBOX_CLANG_EXECUTABLE}
      DEPENDS ${NATIVE_BRIDGE_PROBE_SOURCES}
              ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/build-native-bridge-probe
      VERBATIM)
  add_custom_target(anbox-native-bridge-tests ALL DEPENDS ${NATIVE_BRIDGE_PROBE_APK})
  install(FILES ${NATIVE_BRIDGE_PROBE_APK}
      DESTINATION share/anbox/native-bridge-tests)

  set(SIGNATURE_SPOOF_BUILD_ROOT
      ${CMAKE_CURRENT_BINARY_DIR}/signature-spoof)
  set(SIGNATURE_SPOOF_PERMISSION_APK
      ${SIGNATURE_SPOOF_BUILD_ROOT}/ReboxedSignatureSpoofPermission.apk)
  add_custom_command(
      OUTPUT ${SIGNATURE_SPOOF_PERMISSION_APK}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${SIGNATURE_SPOOF_BUILD_ROOT}
      COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/build-signature-spoof-permission
              ${CMAKE_CURRENT_SOURCE_DIR}/../android/signature-spoof-permission
              ${SIGNATURE_SPOOF_PERMISSION_APK}
              ${ANBOX_ANDROID_SDK_ROOT}
      DEPENDS
              ${CMAKE_CURRENT_SOURCE_DIR}/../android/signature-spoof-permission/AndroidManifest.xml
              ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/build-signature-spoof-permission
      VERBATIM)
  add_custom_target(anbox-signature-spoof-permission ALL
      DEPENDS ${SIGNATURE_SPOOF_PERMISSION_APK})
  install(FILES ${SIGNATURE_SPOOF_PERMISSION_APK}
      DESTINATION share/anbox/signature-spoof)

  set(SIGNATURE_SPOOF_PROBE_APK
      ${SIGNATURE_SPOOF_BUILD_ROOT}/ReboxedSignatureSpoofProbe.apk)
  file(GLOB_RECURSE SIGNATURE_SPOOF_PROBE_SOURCES CONFIGURE_DEPENDS
       ${CMAKE_CURRENT_SOURCE_DIR}/../android/signature-spoof-probe/*)
  add_custom_command(
      OUTPUT ${SIGNATURE_SPOOF_PROBE_APK}
      COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/build-signature-spoof-probe
              ${CMAKE_CURRENT_SOURCE_DIR}/../android/signature-spoof-probe
              ${SIGNATURE_SPOOF_PROBE_APK}
              ${ANBOX_ANDROID_SDK_ROOT}
      DEPENDS ${SIGNATURE_SPOOF_PROBE_SOURCES}
              ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/build-signature-spoof-probe
      VERBATIM)
  add_custom_target(anbox-signature-spoof-probe ALL
      DEPENDS ${SIGNATURE_SPOOF_PROBE_APK})
  install(FILES ${SIGNATURE_SPOOF_PROBE_APK}
      DESTINATION share/anbox/signature-spoof-tests)
endif()
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/../data/signature-spoof/android-15-reboxed-v1.json
    DESTINATION share/anbox/signature-spoof)
install(DIRECTORY
    ${CMAKE_CURRENT_SOURCE_DIR}/../android/signature-spoof-framework/
    DESTINATION share/anbox/signature-spoof/framework-source)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../data/android-gles-compat-v1.json
        DESTINATION share/anbox)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../data/android-gfxstream-compat-v1.json
        DESTINATION share/anbox)
set(ANBOX_GLES_COMPAT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../data/gles-compat"
    CACHE PATH "Optional externally built Android GLES compatibility payload")
if(IS_DIRECTORY "${ANBOX_GLES_COMPAT_DIR}")
  install(DIRECTORY ${ANBOX_GLES_COMPAT_DIR}/
          DESTINATION share/anbox/gles-compat)
else()
  message(STATUS
      "Android GLES replacement payload not supplied; see docs/public-build.md")
endif()
