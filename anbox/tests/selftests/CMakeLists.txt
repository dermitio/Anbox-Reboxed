add_executable(input_pipeline_selftest input_pipeline_selftest.cpp)
target_include_directories(input_pipeline_selftest PRIVATE
                           ${CMAKE_SOURCE_DIR}/src
                           ${CMAKE_SOURCE_DIR})
target_link_directories(input_pipeline_selftest PRIVATE
                        ${CMAKE_BINARY_DIR}/external/sdbus-cpp/src/sdbus-cpp-build)
target_link_libraries(input_pipeline_selftest anbox-core ${Boost_LIBRARIES}
                      ${CMAKE_THREAD_LIBS_INIT})
add_test(NAME input_pipeline_selftest COMMAND input_pipeline_selftest)

add_executable(viewport_lifecycle_selftest viewport_lifecycle_selftest.cpp)
target_include_directories(viewport_lifecycle_selftest PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR})
target_link_directories(viewport_lifecycle_selftest PRIVATE
  ${CMAKE_BINARY_DIR}/external/sdbus-cpp/src/sdbus-cpp-build)
target_link_libraries(viewport_lifecycle_selftest anbox-core ${Boost_LIBRARIES}
  ${CMAKE_THREAD_LIBS_INIT})
add_test(NAME viewport_lifecycle_selftest COMMAND viewport_lifecycle_selftest)

add_executable(single_window_geometry_selftest
  single_window_geometry_selftest.cpp)
target_include_directories(single_window_geometry_selftest PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR})
target_link_directories(single_window_geometry_selftest PRIVATE
  ${CMAKE_BINARY_DIR}/external/sdbus-cpp/src/sdbus-cpp-build)
target_link_libraries(single_window_geometry_selftest anbox-core
  ${Boost_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})
add_test(NAME single_window_geometry_selftest
  COMMAND single_window_geometry_selftest)

# The workspace bootstrap stages LXC rather than installing it system-wide.
# These tests link anbox-core and therefore need the same staged runtime even
# though they do not exercise container code directly.
set(ANBOX_SELFTEST_LXC_LIBRARY_DIR
    "${CMAKE_BINARY_DIR}/host-deps/lxc-root/usr/lib")
if(EXISTS "${ANBOX_SELFTEST_LXC_LIBRARY_DIR}/liblxc.so.1")
  set_target_properties(
    input_pipeline_selftest viewport_lifecycle_selftest
    single_window_geometry_selftest
    PROPERTIES BUILD_RPATH "${ANBOX_SELFTEST_LXC_LIBRARY_DIR}")
endif()

option(ANBOX_BUILD_LOCAL_BERBERIS_TESTS
  "Build unpublished Berberis tests from a local development checkout" OFF)
if(ANBOX_BUILD_LOCAL_BERBERIS_TESTS)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/BerberisLocal.cmake")
    message(FATAL_ERROR "Berberis is not included in the public devalpha release.")
  endif()
  include("${CMAKE_CURRENT_SOURCE_DIR}/BerberisLocal.cmake")
endif()

find_program(PYTHON3_EXECUTABLE python3)
if(PYTHON3_EXECUTABLE)
  add_test(
    NAME public_installer_selftest
    COMMAND ${PYTHON3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/public_installer_selftest.py
            ${CMAKE_SOURCE_DIR}/../anbox-reboxed-install.sh)
  find_program(CLANGXX_EXECUTABLE NAMES clang++)
  if(ANBOX_BUILD_LOCAL_BERBERIS_TESTS AND CLANGXX_EXECUTABLE AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND
     CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    add_test(
      NAME berberis_arm64_machine_ir_selftest
      COMMAND ${PYTHON3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/berberis_arm64_machine_ir_selftest.py
              ${CMAKE_SOURCE_DIR}/.. ${CLANGXX_EXECUTABLE})
  endif()
  add_test(
    NAME gfxstream_compat_selftest
    COMMAND ${PYTHON3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/gfxstream_compat_selftest.py
            ${CMAKE_SOURCE_DIR}/scripts/anbox-gfxstream-compat)
  add_test(
    NAME gfxstream_host_patch_selftest
    COMMAND ${PYTHON3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/gfxstream_host_patch_selftest.py
            ${CMAKE_SOURCE_DIR}/patches/gfxstream-colorbuffer-texture-sync.patch)
  add_test(
    NAME native_bridge_selftest
    COMMAND ${PYTHON3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/native_bridge_selftest.py
            ${CMAKE_SOURCE_DIR}/scripts/anbox-native-bridge)
  if(ANBOX_BUILD_LOCAL_BERBERIS_TESTS)
    add_test(
      NAME translation_sources_selftest
      COMMAND ${PYTHON3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/translation_sources_selftest.py
              ${CMAKE_SOURCE_DIR}/scripts/prepare-android-translation-sources)
  endif()
  add_test(
    NAME data_image_selftest
    COMMAND ${PYTHON3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/data_image_selftest.py
            ${CMAKE_SOURCE_DIR}/scripts/anbox-data-image)
  add_test(
    NAME signature_spoof_selftest
    COMMAND ${PYTHON3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/signature_spoof_selftest.py
            ${CMAKE_SOURCE_DIR}/scripts/anbox-signature-spoof)
  add_test(
    NAME reboxed_bridge_selftest
    COMMAND ${PYTHON3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/reboxed_bridge_selftest.py
            ${CMAKE_SOURCE_DIR}/scripts/anbox-reboxed-bridge)
endif()
