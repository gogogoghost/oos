cmake_minimum_required(VERSION 3.22)
project(orange_os C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SYSTEM_DIR "$ENV{SYSTEM_DIR}" CACHE PATH
    "Absolute path to the extracted Android /system directory")
set(THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party" CACHE PATH
    "Directory containing external AOSP, Gecko, and KaiOS checkouts")
set(HIDL_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/generated/hidl-android10"
    CACHE PATH "Generated Android 10 HIDL headers")

option(BUILD_NOKIA_2780_FLIP "Build Nokia 2780 Flip platform support" ON)
option(BUILD_NOKIA_8110_4G "Build Nokia 8110 4G platform support" OFF)
option(OOS_BUILD_DEVICE_TESTS "Build on-device display tests" ON)

if(BUILD_NOKIA_2780_FLIP AND BUILD_NOKIA_8110_4G)
  message(FATAL_ERROR "Select exactly one target device per build directory.")
endif()
if(NOT BUILD_NOKIA_2780_FLIP AND NOT BUILD_NOKIA_8110_4G)
  message(FATAL_ERROR "No target device selected.")
endif()

if(BUILD_NOKIA_8110_4G)
  # Android 6 libc++ predates the C++17 aligned allocation entry points.
  add_compile_definitions(_LIBCPP_HAS_NO_ALIGNED_ALLOCATION)
endif()

if(NOT SYSTEM_DIR OR NOT EXISTS "${SYSTEM_DIR}/lib/libc++.so")
  message(FATAL_ERROR
    "SYSTEM_DIR must point to an extracted Android /system directory containing lib/libc++.so. "
    "Set it in .env and configure with scripts/configure-android.sh.")
endif()
if(BUILD_NOKIA_2780_FLIP)
  if(NOT EXISTS "${THIRD_PARTY_DIR}/gecko-b2g/widget/gonk/hwchal/android_10/HWC2.cpp")
    message(FATAL_ERROR "Third-party source checkouts are missing under ${THIRD_PARTY_DIR}.")
  endif()
  if(NOT EXISTS "${HIDL_GENERATED_DIR}/android/hardware/graphics/composer/2.3/IComposer.h")
    message(FATAL_ERROR
      "Generated HIDL headers are missing. Run scripts/generate-hidl-headers.sh first.")
  endif()
endif()

add_subdirectory(runtime/wamr)

add_library(oos_input STATIC core/src/key_input.cpp)
target_include_directories(oos_input PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/core/include")

add_library(oos_device_api STATIC core/src/device.cpp)
target_include_directories(oos_device_api PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/core/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/sdk/include")

add_library(oos_compositor STATIC core/src/compositor.cpp)
target_include_directories(oos_compositor PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/core/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/sdk/include")
target_link_libraries(oos_compositor PUBLIC oos_device_api)
add_library(oos::compositor ALIAS oos_compositor)

add_library(oos_surface_transport STATIC core/src/surface_transport.c)
target_include_directories(oos_surface_transport PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/core/include")

add_library(oos_wasm_runtime STATIC
  core/src/native_app_manager.cpp
  core/src/wasm_app.cpp)
target_include_directories(oos_wasm_runtime PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/core/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/sdk/include")
target_link_libraries(oos_wasm_runtime PUBLIC oos_wamr oos_input)

add_executable(oos app/main.cpp app/runtime.cpp)
target_compile_options(oos PRIVATE -fno-exceptions -fno-rtti)
target_link_options(oos PRIVATE -nostdlib++)
target_link_libraries(oos PRIVATE oos_device_api oos_compositor)
set_target_properties(oos PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(BUILD_NOKIA_2780_FLIP)
  add_subdirectory(devices/nokia-2780-flip)
endif()
if(BUILD_NOKIA_8110_4G)
  add_subdirectory(devices/nokia-8110-4g)
endif()
