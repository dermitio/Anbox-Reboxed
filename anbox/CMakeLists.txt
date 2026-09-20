cmake_minimum_required(VERSION 3.10)
project(anbox_reboxed C CXX)

add_definitions(-DANBOX_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\")

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR
    "In-source builds are not supported. Use: cmake -S . -B build")
endif()

if(POLICY CMP0167)
  cmake_policy(SET CMP0167 OLD)
endif()

set(Boost_NO_BOOST_CMAKE ON)

include(CTest)
include(GNUInstallDirs)

if (NOT CMAKE_BUILD_TYPE)
    message(STATUS "No build type selected, default to RelWithDebInfo")
    set(CMAKE_BUILD_TYPE "RelWithDebInfo")
endif()

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -pedantic -Wno-error=pedantic -Wno-variadic-macros -Wextra -fPIC")

# By default, all symbols are visible in the library. We strip out things we don't
# want at link time, by running a version script (see unity-scopes.map and the
# setting of LINK_FLAGS below).
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fvisibility=default")

set(C_AND_CXX_WARNINGS "-pedantic -Wno-error=pedantic -Wall -Wextra")

# Some additional warnings not included by the general flags set above.
set(EXTRA_C_WARNINGS "-Wcast-align -Wcast-qual -Wformat -Wredundant-decls -Wswitch-default")
set(EXTRA_CXX_WARNINGS "-Wnon-virtual-dtor -Wold-style-cast")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${C_AND_CXX_WARNINGS}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${EXTRA_C_WARNINGS}")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17 ${C_AND_CXX_WARNINGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${EXTRA_C_WARNINGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${EXTRA_CXX_WARNINGS}")

# -fno-permissive causes warnings with clang, so we only enable it for gcc
if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-permissive")
endif()

string(TOLOWER "${CMAKE_BUILD_TYPE}" cmake_build_type_lower)

if ("${cmake_build_type_lower}" STREQUAL "release" OR "${cmake_build_type_lower}" STREQUAL "relwithdebinfo")
  option(Werror "Treat warnings as errors" ON)
else()
  option(Werror "Treat warnings as errors" OFF)
endif()

if (${Werror})
    message(STATUS "Treat warnings as errors")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Werror")
  if ("${cmake_build_type_lower}" STREQUAL "release" OR "${cmake_build_type_lower}" STREQUAL "relwithdebinfo")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-error=deprecated-declarations")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-error=deprecated-declarations")
  endif()
endif()

set(CMAKE_INCLUDE_CURRENT_DIR ON)

find_package(Boost COMPONENTS filesystem log serialization thread program_options)
find_package(PkgConfig)
find_package(Threads)
find_package(EGL REQUIRED)
find_package(GLESv2 REQUIRED)
find_package(Protobuf REQUIRED)

pkg_check_modules(SDL2 sdl2 REQUIRED)
pkg_check_modules(SDL2_IMAGE SDL2_image REQUIRED)
pkg_check_modules(DBUS dbus-1 REQUIRED)
pkg_check_modules(LXC lxc REQUIRED)
pkg_check_modules(LIBSYSTEMD libsystemd REQUIRED)
pkg_check_modules(LIBCAP libcap REQUIRED)
pkg_check_modules(GFXSTREAM gfxstream_backend)

set(GFXSTREAM_BACKEND_LIBRARY "" CACHE FILEPATH
    "Exact libgfxstream_backend shared library matching the Android guest encoder")
set(GFXSTREAM_PROTOCOL_REVISION "unknown" CACHE STRING
    "Guest/host gfxstream protocol source revision used for diagnostics")

if (GFXSTREAM_BACKEND_LIBRARY)
  if (NOT EXISTS "${GFXSTREAM_BACKEND_LIBRARY}")
    message(FATAL_ERROR
      "GFXSTREAM_BACKEND_LIBRARY does not exist: ${GFXSTREAM_BACKEND_LIBRARY}")
  endif()
  get_filename_component(GFXSTREAM_BACKEND_LIBRARY
      "${GFXSTREAM_BACKEND_LIBRARY}" REALPATH)
  set(GFXSTREAM_FOUND TRUE)
  set(GFXSTREAM_LIBRARIES "${GFXSTREAM_BACKEND_LIBRARY}")
  set(GFXSTREAM_LDFLAGS "")
  message(STATUS
    "Using pinned gfxstream backend: ${GFXSTREAM_BACKEND_LIBRARY} (protocol ${GFXSTREAM_PROTOCOL_REVISION})")
endif()

if (GFXSTREAM_FOUND)
  set(GFXSTREAM_RENDER_UTILS_INCLUDE_DIR "" CACHE PATH
      "Directory containing gfxstream's render-utils directory")
  set(_gfxstream_interface_candidates
      "${GFXSTREAM_RENDER_UTILS_INCLUDE_DIR}")
  foreach(_gfxstream_include ${GFXSTREAM_INCLUDE_DIRS})
    list(APPEND _gfxstream_interface_candidates
         "${_gfxstream_include}"
         "${_gfxstream_include}/include")
  endforeach()
  list(APPEND _gfxstream_interface_candidates
       "${CMAKE_SOURCE_DIR}/third_party/gfxstream/include")
  unset(GFXSTREAM_INTERFACE_SOURCE)
  foreach(_gfxstream_candidate ${_gfxstream_interface_candidates})
    if (_gfxstream_candidate AND
        EXISTS "${_gfxstream_candidate}/render-utils/Renderer.h" AND
        EXISTS "${_gfxstream_candidate}/render-utils/RenderChannel.h")
      set(GFXSTREAM_INTERFACE_SOURCE "${_gfxstream_candidate}/render-utils")
      break()
    endif()
  endforeach()

  if (GFXSTREAM_INTERFACE_SOURCE)
    message(STATUS "Modern gfxstream backend ${GFXSTREAM_VERSION} enabled (render-utils: ${GFXSTREAM_INTERFACE_SOURCE})")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DANBOX_HAVE_GFXSTREAM")
    add_definitions(-DANBOX_GFXSTREAM_PROTOCOL_REVISION=\"${GFXSTREAM_PROTOCOL_REVISION}\")
    set(GFXSTREAM_INTERFACE_INCLUDE
        "${CMAKE_BINARY_DIR}/generated/gfxstream-interface")
    file(MAKE_DIRECTORY "${GFXSTREAM_INTERFACE_INCLUDE}/render-utils")
    foreach(GFXSTREAM_INTERFACE_HEADER
            Renderer.h RenderChannel.h render_api_platform_types.h
            virtio_gpu_ops.h)
      configure_file(
          "${GFXSTREAM_INTERFACE_SOURCE}/${GFXSTREAM_INTERFACE_HEADER}"
          "${GFXSTREAM_INTERFACE_INCLUDE}/render-utils/${GFXSTREAM_INTERFACE_HEADER}"
          COPYONLY)
    endforeach()
  else()
    message(WARNING
      "gfxstream_backend was found without its render-utils development headers; modern Vulkan transport disabled. Set GFXSTREAM_RENDER_UTILS_INCLUDE_DIR to the gfxstream include directory.")
    set(GFXSTREAM_FOUND FALSE)
  endif()
else()
  message(STATUS "Modern gfxstream backend not found; Vulkan transport disabled")
endif()

message(STATUS "LXC version: ${LXC_VERSION}")
if (${LXC_VERSION} VERSION_LESS 3.0)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DENABLE_LXC2_SUPPORT")
  message(STATUS "Building with LXC 2.x compatibility support")
endif()

option(ENABLE_X11 "Enable X11 support" ON)
if (ENABLE_X11)
  find_package(X11 REQUIRED)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DX11_SUPPORT")
endif()

option(ENABLE_WAYLAND "Enable wayland support" ON)
if (ENABLE_WAYLAND)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DWAYLAND_SUPPORT")
endif()

option(ENABLE_MIR "Enable mir support" OFF)
if (ENABLE_MIR)
  pkg_check_modules(MIRCLIENT mirclient REQUIRED)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DMIR_SUPPORT")
endif()

if (NOT BINDERFS_PATH)
  set(BINDERFS_PATH "/dev/binderfs")
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DEGL_NO_X11")

if((Protobuf_VERSION VERSION_GREATER "3.7") OR (Protobuf_VERSION VERSION_EQUAL "3.7"))
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DUSE_PROTOBUF_CALLBACK_HEADER")
endif()

#####################################################################
# Enable code coverage calculation with gcov/gcovr/lcov
# Usage:
#  * Switch build type to coverage (use ccmake or cmake-gui)
#  * Invoke make, make test, make coverage
#  * Find html report in subdir coveragereport
#  * Find xml report feasible for jenkins in coverage.xml
#####################################################################
IF(CMAKE_BUILD_TYPE MATCHES [cC][oO][vV][eE][rR][aA][gG][eE])
  SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ftest-coverage -fprofile-arcs" )
  SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ftest-coverage -fprofile-arcs" )
  SET(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -ftest-coverage -fprofile-arcs" )
  SET(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -ftest-coverage -fprofile-arcs" )
ENDIF(CMAKE_BUILD_TYPE MATCHES [cC][oO][vV][eE][rR][aA][gG][eE])

find_package(GMock QUIET)
find_package(GTest QUIET)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17 -fPIC")


if (NOT ANBOX_VERSION)
    set(ANBOX_VERSION "0.0.1-devalpha")
endif()
if (ANBOX_VERSION_SUFFIX)
    set(ANBOX_VERSION "${ANBOX_VERSION}-${ANBOX_VERSION_SUFFIX}")
endif()
set(ANBOX_RESOURCE_DIR "${CMAKE_INSTALL_DATADIR}/anbox")
set(ANBOX_RESOURCE_DIR_FULL "${CMAKE_INSTALL_FULL_DATADIR}/anbox")
set(ANBOX_STATEDIR_FULL "${CMAKE_INSTALL_FULL_LOCALSTATEDIR}/lib/anbox")
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/anbox/build/config.h.in
               ${CMAKE_CURRENT_SOURCE_DIR}/src/anbox/build/config.h)

add_subdirectory(external)
add_subdirectory(src)
if(BUILD_TESTING)
  add_subdirectory(tests/selftests)
  if((GMock_FOUND OR GMOCK_FOUND) AND (GTest_FOUND OR GTEST_FOUND))
    add_subdirectory(tests)
  else()
    message(STATUS "Skipping tests because GMock/GTest was not found")
  endif()
endif()
add_subdirectory(android)

if (NOT "${HOST_CMAKE_C_COMPILER}" STREQUAL "")
  message(STATUS "Host C compiler: ${HOST_CMAKE_C_COMPILER}")
  message(STATUS "Host C compiler: ${HOST_CMAKE_CXX_COMPILER}")
endif()

option(SNAP_CONFINEMENT "Enable snap confinement support" OFF)
if (SNAP_CONFINEMENT)
  message(STATUS "Building with support for snap confinement")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DENABLE_SNAP_CONFINEMENT")
endif()

install(FILES data/ui/loading-screen.png DESTINATION ${ANBOX_RESOURCE_DIR}/ui)
install(FILES data/99-anbox-virtual-input.rules
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/udev/rules.d)
install(FILES data/systemd/anbox-reboxed-bridge.service
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/user)
install(FILES docs/reboxed-bridge.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR})

# uninstall target
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake_uninstall.cmake.in"
               "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
               IMMEDIATE @ONLY)
add_custom_target(uninstall "${CMAKE_COMMAND}"
                  -P "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake")
