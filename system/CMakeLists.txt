cmake_minimum_required(VERSION 3.22)
project(orange_os C CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SYSTEM_DIR "$ENV{SYSTEM_DIR}" CACHE PATH
    "Absolute path to the extracted Android /system directory")
get_filename_component(OOS_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(THIRD_PARTY_DIR "${OOS_REPO_ROOT}/third_party" CACHE PATH
    "Directory containing external AOSP and device reference checkouts")
set(HIDL_GENERATED_DIR "${OOS_REPO_ROOT}/build/generated/hidl-android10"
    CACHE PATH "Generated Android 10 HIDL headers")

option(BUILD_NOKIA_2780_FLIP "Build Nokia 2780 Flip platform support" ON)
option(BUILD_NOKIA_8110_4G "Build Nokia 8110 4G platform support" OFF)
option(BUILD_LOCAL "Build the local SDL/OpenGL test platform" OFF)
option(OOS_BUILD_DEVICE_TESTS "Build on-device display tests" ON)

set(OOS_SELECTED_DEVICE_COUNT 0)
foreach(OOS_DEVICE_OPTION BUILD_NOKIA_2780_FLIP BUILD_NOKIA_8110_4G BUILD_LOCAL)
  if(${OOS_DEVICE_OPTION})
    math(EXPR OOS_SELECTED_DEVICE_COUNT "${OOS_SELECTED_DEVICE_COUNT}+1")
  endif()
endforeach()
if(OOS_SELECTED_DEVICE_COUNT GREATER 1)
  message(FATAL_ERROR "Select exactly one target device per build directory.")
endif()
if(OOS_SELECTED_DEVICE_COUNT EQUAL 0)
  message(FATAL_ERROR "No target device selected.")
endif()

if(BUILD_NOKIA_8110_4G)
  # Android 6 libc++ predates the C++17 aligned allocation entry points.
  add_compile_definitions(_LIBCPP_HAS_NO_ALIGNED_ALLOCATION)
endif()
if(NOT BUILD_LOCAL)
  # Device binaries link the stock KaiOS libc++, whose ABI namespace is __1.
  # Keep generic libraries and device adapters on the same C++ ABI.
  add_compile_definitions(_LIBCPP_ABI_NAMESPACE=__1)
endif()

if(NOT BUILD_LOCAL AND (NOT SYSTEM_DIR OR NOT EXISTS "${SYSTEM_DIR}/lib/libc++.so"))
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

add_library(oos_input STATIC src/oos/input/key_input.cpp)
target_include_directories(oos_input PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_library(oos_device_api STATIC src/oos/device/device.cpp)
target_include_directories(oos_device_api PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_library(oos_gles_executor STATIC src/oos/runtime/gles_executor.cpp)
target_include_directories(oos_gles_executor PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_library(oos_compositor STATIC src/oos/compositor/compositor.cpp)
target_include_directories(oos_compositor PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(oos_compositor PUBLIC oos_device_api)
add_library(oos::compositor ALIAS oos_compositor)

add_library(oos_app_model STATIC
  src/oos/apps/app_manifest.cpp
  src/oos/apps/app_repository.cpp
  src/oos/apps/json.cpp
  src/oos/apps/zip_archive.cpp
  src/oos/storage/app_storage.cpp
  src/oos/storage/filesystem.cpp
  src/oos/storage/sqlite.cpp)
target_include_directories(oos_app_model PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(oos_app_model PUBLIC ${CMAKE_DL_LIBS} z)
add_library(oos::apps ALIAS oos_app_model)

add_library(oos_device_storage STATIC
  src/oos/storage/device_storage.cpp)
target_include_directories(oos_device_storage PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(oos_device_storage PUBLIC oos_app_model)

add_library(oos_font_assets STATIC
  src/oos/resources/font_assets.cpp)
target_include_directories(oos_font_assets PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_library(oos_system_services STATIC
  src/oos/services/system_service.cpp)
target_include_directories(oos_system_services PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(oos_system_services PUBLIC oos_app_model)

add_library(oos_wasm_runtime STATIC
  src/oos/runtime/native_app_manager.cpp
  src/oos/runtime/wasm_app.cpp)
target_include_directories(oos_wasm_runtime PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(oos_wasm_runtime PUBLIC
  oos_wamr oos_input oos_app_model oos_device_storage oos_font_assets
  oos_system_services)

set(OOS_BOOT_SPLASH_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/boot/nokia-2780-flip/boot-splash.png")
if(NOT EXISTS "${OOS_BOOT_SPLASH_SOURCE}")
  message(FATAL_ERROR "Missing embedded boot splash: ${OOS_BOOT_SPLASH_SOURCE}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${OOS_BOOT_SPLASH_SOURCE}")
file(READ "${OOS_BOOT_SPLASH_SOURCE}" OOS_BOOT_SPLASH_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1,"
  OOS_BOOT_SPLASH_BYTES "${OOS_BOOT_SPLASH_HEX}")
set(OOS_GENERATED_RESOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/resources")
file(MAKE_DIRECTORY "${OOS_GENERATED_RESOURCE_DIR}")
configure_file(
  src/oos/resources/embedded_boot_splash.cpp.in
  "${OOS_GENERATED_RESOURCE_DIR}/embedded_boot_splash.cpp"
  @ONLY)
unset(OOS_BOOT_SPLASH_HEX)
unset(OOS_BOOT_SPLASH_BYTES)

add_executable(oos
  src/main.cpp
  src/runtime.cpp
  "${OOS_GENERATED_RESOURCE_DIR}/embedded_boot_splash.cpp")
target_compile_options(oos PRIVATE -fno-exceptions -fno-rtti)
if(NOT BUILD_LOCAL)
  target_link_options(oos PRIVATE -nostdlib++)
endif()
target_link_libraries(oos PRIVATE
  oos_device_api oos_compositor oos_app_model oos_device_storage)
set_target_properties(oos PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(BUILD_NOKIA_2780_FLIP)
  add_subdirectory(devices/nokia-2780-flip)
endif()
if(BUILD_NOKIA_8110_4G)
  add_subdirectory(devices/nokia-8110-4g)
endif()
if(BUILD_LOCAL)
  add_subdirectory(devices/local)
endif()
