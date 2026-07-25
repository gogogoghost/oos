cmake_minimum_required(VERSION 3.22)
project(kaios_display_poc C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SYSTEM_DIR "$ENV{SYSTEM_DIR}" CACHE PATH
    "Absolute path to the extracted Android /system directory")
set(THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party" CACHE PATH
    "Directory containing external AOSP, Gecko, and KaiOS checkouts")
set(HIDL_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/generated/hidl-android10"
    CACHE PATH "Generated Android 10 HIDL headers")

if(NOT SYSTEM_DIR OR NOT EXISTS "${SYSTEM_DIR}/lib/libc++.so")
  message(FATAL_ERROR
    "SYSTEM_DIR must point to an extracted Android /system directory containing lib/libc++.so. "
    "Set it in .env and configure with scripts/configure-android.sh.")
endif()
if(NOT EXISTS "${THIRD_PARTY_DIR}/gecko-b2g/widget/gonk/hwchal/android_10/HWC2.cpp")
  message(FATAL_ERROR "Third-party source checkouts are missing under ${THIRD_PARTY_DIR}.")
endif()
if(NOT EXISTS "${HIDL_GENERATED_DIR}/android/hardware/graphics/composer/2.3/IComposer.h")
  message(FATAL_ERROR
    "Generated HIDL headers are missing. Run scripts/generate-hidl-headers.sh first.")
endif()

option(BUILD_NOKIA_2780_FLIP "Build Nokia 2780 Flip POCs" ON)
option(BUILD_NOKIA_8110_4G "Enable the Nokia 8110 4G device slot" OFF)

if(BUILD_NOKIA_2780_FLIP)
  add_subdirectory(devices/nokia-2780-flip)
endif()
if(BUILD_NOKIA_8110_4G)
  add_subdirectory(devices/nokia-8110-4g)
endif()
