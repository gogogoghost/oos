set(NOKIA_2780_SYSTEM_LIB_DIR "${SYSTEM_DIR}/lib")
set(NOKIA_2780_HWC_DIR
    "${THIRD_PARTY_DIR}/gecko-b2g/widget/gonk/hwchal/android_10")
set(NOKIA_2780_WPE_SYSROOT
    "${CMAKE_SOURCE_DIR}/build/wpe-sysroot/nokia-2780-flip")
set(NOKIA_2780_RADIO_IMPL_DIR
    "${CMAKE_SOURCE_DIR}/build/generated/hidl-android10-impl/radio-1.0")

set(NOKIA_2780_ANDROID_INCLUDES
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${HIDL_GENERATED_DIR}"
  "${THIRD_PARTY_DIR}/aosp-hardware-interfaces/graphics/common/1.0"
  "${THIRD_PARTY_DIR}/aosp-hardware-interfaces/graphics/common/1.1"
  "${THIRD_PARTY_DIR}/aosp-hardware-interfaces/graphics/common/1.2"
  "${THIRD_PARTY_DIR}/aosp-hardware-interfaces/graphics/composer/2.1/utils/command-buffer/include"
  "${THIRD_PARTY_DIR}/aosp-hardware-interfaces/graphics/composer/2.2/utils/command-buffer/include"
  "${THIRD_PARTY_DIR}/aosp-hardware-interfaces/graphics/composer/2.3/utils/command-buffer/include"
  "${THIRD_PARTY_DIR}/aosp-frameworks-native/include"
  "${THIRD_PARTY_DIR}/aosp-frameworks-native/libs/binder/include"
  "${THIRD_PARTY_DIR}/aosp-frameworks-native/libs/gui/include"
  "${THIRD_PARTY_DIR}/aosp-frameworks-native/libs/nativewindow/include"
  "${THIRD_PARTY_DIR}/aosp-frameworks-native/libs/ui/include"
  "${THIRD_PARTY_DIR}/aosp-frameworks-native/libs/math/include"
  "${THIRD_PARTY_DIR}/aosp-frameworks-native/libs/nativebase/include"
  "${THIRD_PARTY_DIR}/aosp-system-core/base/include"
  "${THIRD_PARTY_DIR}/aosp-system-core/libcutils/include"
  "${THIRD_PARTY_DIR}/aosp-system-core/liblog/include"
  "${THIRD_PARTY_DIR}/aosp-system-core/libsync/include"
  "${THIRD_PARTY_DIR}/aosp-system-core/libsystem/include"
  "${THIRD_PARTY_DIR}/aosp-system-core/libutils/include"
  "${THIRD_PARTY_DIR}/aosp-system-core/libsuspend/include"
  "${THIRD_PARTY_DIR}/aosp-system-libhidl/base/include"
  "${THIRD_PARTY_DIR}/aosp-system-libhidl/transport/include"
  "${THIRD_PARTY_DIR}/aosp-system-libhidl/transport/token/1.0/utils/include"
  "${THIRD_PARTY_DIR}/aosp-system-libfmq/include"
  "${THIRD_PARTY_DIR}/aosp-hardware-libhardware/include"
  "${NOKIA_2780_HWC_DIR}")

set(NOKIA_2780_WPE_INCLUDES
  "${NOKIA_2780_WPE_SYSROOT}/include/wpe-webkit-2.0"
  "${NOKIA_2780_WPE_SYSROOT}/include/wpe-webkit-2.0/wpe-platform"
  "${NOKIA_2780_WPE_SYSROOT}/include/wpe-1.0"
  "${NOKIA_2780_WPE_SYSROOT}/include"
  "${NOKIA_2780_WPE_SYSROOT}/include/glib-2.0"
  "${NOKIA_2780_WPE_SYSROOT}/include/gio-unix-2.0"
  "${NOKIA_2780_WPE_SYSROOT}/include/libsoup-3.0"
  "${NOKIA_2780_WPE_SYSROOT}/lib/glib-2.0/include"
  "${THIRD_PARTY_DIR}/aosp-frameworks-native/libs/nativewindow/include-private")

set(NOKIA_2780_SYSTEM_LIBRARIES
  "${NOKIA_2780_SYSTEM_LIB_DIR}/libc++.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/android.hardware.media@1.0.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/android.hardware.power@1.0.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/android.hardware.graphics.bufferqueue@2.0.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/android.hardware.graphics.composer@2.1.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/android.hardware.graphics.composer@2.2.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/android.hardware.graphics.composer@2.3.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/liblog.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/libcutils.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/libutils.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/libhwbinder.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/libfmq.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/libbase.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/libui.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/libgui.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/libbinder.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/libhidlbase.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/libhidltransport.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/libnativewindow.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/libhardware.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/libsync.so")

function(oos_nokia_2780_android_target target)
  target_compile_definitions(${target} PRIVATE
    ANDROID_VERSION=29
    _LIBCPP_ABI_NAMESPACE=__1)
  target_compile_options(${target} PRIVATE
    -include "${CMAKE_CURRENT_SOURCE_DIR}/include/oos/nokia2780/android_compat.h"
    -fno-rtti)
  target_include_directories(${target} PRIVATE ${NOKIA_2780_ANDROID_INCLUDES})
  target_link_options(${target} PRIVATE
    -nostdlib++
    "-Wl,-rpath-link,${NOKIA_2780_SYSTEM_LIB_DIR}")
endfunction()

add_library(oos_nokia_2780_display STATIC
  src/display_control.cpp
  "${NOKIA_2780_HWC_DIR}/HWC2.cpp"
  "${NOKIA_2780_HWC_DIR}/ComposerHal.cpp")
oos_nokia_2780_android_target(oos_nokia_2780_display)
target_include_directories(oos_nokia_2780_display PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(oos_nokia_2780_display PUBLIC
  ${NOKIA_2780_SYSTEM_LIBRARIES})

add_library(oos_nokia_2780_wpe_display STATIC
  src/wpe_display_manager.cpp)
oos_nokia_2780_android_target(oos_nokia_2780_wpe_display)
target_include_directories(oos_nokia_2780_wpe_display PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(oos_nokia_2780_wpe_display PRIVATE
  ${NOKIA_2780_WPE_INCLUDES})
target_link_libraries(oos_nokia_2780_wpe_display PUBLIC
  oos_nokia_2780_display
  "${NOKIA_2780_WPE_SYSROOT}/lib/libWPEBackend-android.so"
  EGL GLESv2 android)

target_sources(oos PRIVATE src/oos_runtime.cpp)
oos_nokia_2780_android_target(oos)
target_include_directories(oos PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_SOURCE_DIR}/core/include"
  ${NOKIA_2780_WPE_INCLUDES})
target_link_libraries(oos PRIVATE
  oos_input
  oos_nokia_2780_wpe_display
  "${NOKIA_2780_WPE_SYSROOT}/lib/libWPEWebKit-2.0.so"
  "${NOKIA_2780_WPE_SYSROOT}/lib/libglib-2.0.so"
  "${NOKIA_2780_WPE_SYSROOT}/lib/libgobject-2.0.so"
  "${NOKIA_2780_WPE_SYSROOT}/lib/libpng16.so")

add_library(oos_nokia_2780_network STATIC
  "${CMAKE_SOURCE_DIR}/core/src/wifi_manager.cpp"
  "${CMAKE_SOURCE_DIR}/core/src/ip_manager.cpp"
  "${CMAKE_SOURCE_DIR}/core/src/bluetooth_manager.cpp")
oos_nokia_2780_android_target(oos_nokia_2780_network)
target_include_directories(oos_nokia_2780_network PUBLIC
  "${CMAKE_SOURCE_DIR}/core/include")
target_link_libraries(oos_nokia_2780_network PUBLIC
  "${NOKIA_2780_SYSTEM_LIB_DIR}/libc++.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/libcutils.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/libnetutils.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/liblog.so")

add_library(oos_nokia_2780_hardware STATIC
  "${CMAKE_SOURCE_DIR}/core/src/audio_manager.cpp"
  "${CMAKE_SOURCE_DIR}/core/src/codec_manager.cpp"
  src/camera_manager.cpp
  src/power_manager.cpp
  src/vibrator_manager.cpp)
oos_nokia_2780_android_target(oos_nokia_2780_hardware)
target_include_directories(oos_nokia_2780_hardware PUBLIC
  "${CMAKE_SOURCE_DIR}/core/include")
target_link_libraries(oos_nokia_2780_hardware PUBLIC
  "${NOKIA_2780_SYSTEM_LIB_DIR}/libc++.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/libaaudio.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/libmediandk.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/android.hardware.camera.common@1.0.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/android.hardware.camera.device@1.0.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/android.hardware.camera.provider@2.4.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/libsuspend.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/android.hardware.power@1.0.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/android.hardware.vibrator@1.0.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/libhidlbase.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/libhidltransport.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/libhwbinder.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/libutils.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/libgui.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/libui.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/libnativewindow.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/libbinder.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/libcutils.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/liblog.so")

add_library(oos_nokia_2780_modem STATIC
  "${CMAKE_SOURCE_DIR}/core/src/modem_manager.cpp"
  "${NOKIA_2780_RADIO_IMPL_DIR}/RadioResponse.cpp"
  "${NOKIA_2780_RADIO_IMPL_DIR}/RadioIndication.cpp")
oos_nokia_2780_android_target(oos_nokia_2780_modem)
target_include_directories(oos_nokia_2780_modem PUBLIC
  "${CMAKE_SOURCE_DIR}/core/include")
target_include_directories(oos_nokia_2780_modem PRIVATE
  "${NOKIA_2780_RADIO_IMPL_DIR}")
target_link_libraries(oos_nokia_2780_modem PUBLIC
  "${NOKIA_2780_SYSTEM_LIB_DIR}/libc++.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/android.hardware.radio@1.0.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/libhidlbase.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/libhidltransport.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/libhwbinder.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/libutils.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/libcutils.so"
  "${NOKIA_2780_SYSTEM_LIB_DIR}/liblog.so")

if(OOS_BUILD_DEVICE_TESTS)
  add_executable(oos_test_nokia_2780_primary_gl
    tests/primary_gl_animation_test.cpp)
  oos_nokia_2780_android_target(oos_test_nokia_2780_primary_gl)
  target_link_libraries(oos_test_nokia_2780_primary_gl PRIVATE
    oos_nokia_2780_display EGL GLESv2 android)

  add_executable(oos_test_nokia_2780_primary_bufferqueue
    tests/experiments/primary_bufferqueue_test.cpp)
  oos_nokia_2780_android_target(oos_test_nokia_2780_primary_bufferqueue)
  target_link_libraries(oos_test_nokia_2780_primary_bufferqueue PRIVATE
    oos_nokia_2780_display EGL GLESv2 android)

  add_executable(oos_test_nokia_2780_cover_green
    tests/cover_green_test.cpp)
  oos_nokia_2780_android_target(oos_test_nokia_2780_cover_green)
  target_link_libraries(oos_test_nokia_2780_cover_green PRIVATE
    oos_nokia_2780_display)

  add_executable(oos_test_nokia_2780_cover_secondary
    tests/cover_secondary_test.cpp
    tests/support/cover_fixture.cpp)
  oos_nokia_2780_android_target(oos_test_nokia_2780_cover_secondary)
  target_include_directories(oos_test_nokia_2780_cover_secondary PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/support")
  target_link_libraries(oos_test_nokia_2780_cover_secondary PRIVATE
    oos_nokia_2780_display)

  add_executable(oos_test_nokia_2780_wpe_display
    tests/wpe_display_test.cpp
    tests/support/cover_fixture.cpp)
  oos_nokia_2780_android_target(oos_test_nokia_2780_wpe_display)
  target_include_directories(oos_test_nokia_2780_wpe_display PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/support"
    ${NOKIA_2780_WPE_INCLUDES})
  target_link_libraries(oos_test_nokia_2780_wpe_display PRIVATE
    oos_nokia_2780_wpe_display
    "${NOKIA_2780_WPE_SYSROOT}/lib/libWPEWebKit-2.0.so"
    "${NOKIA_2780_WPE_SYSROOT}/lib/libglib-2.0.so"
    "${NOKIA_2780_WPE_SYSROOT}/lib/libgobject-2.0.so")

  add_executable(oos_test_nokia_2780_key_input
    tests/key_input_test.cpp)
  oos_nokia_2780_android_target(oos_test_nokia_2780_key_input)
  target_include_directories(oos_test_nokia_2780_key_input PRIVATE
    ${NOKIA_2780_WPE_INCLUDES})
  target_link_libraries(oos_test_nokia_2780_key_input PRIVATE
    oos_input
    oos_nokia_2780_wpe_display
    "${NOKIA_2780_WPE_SYSROOT}/lib/libWPEWebKit-2.0.so"
    "${NOKIA_2780_WPE_SYSROOT}/lib/libglib-2.0.so"
    "${NOKIA_2780_WPE_SYSROOT}/lib/libgobject-2.0.so")

  add_executable(oos_test_nokia_2780_network_headless
    tests/network_headless_test.cpp)
  oos_nokia_2780_android_target(oos_test_nokia_2780_network_headless)
  target_link_libraries(oos_test_nokia_2780_network_headless PRIVATE
    oos_nokia_2780_network)

  add_executable(oos_test_nokia_2780_modem_headless
    tests/modem_headless_test.cpp)
  oos_nokia_2780_android_target(oos_test_nokia_2780_modem_headless)
  target_link_libraries(oos_test_nokia_2780_modem_headless PRIVATE
    oos_nokia_2780_modem)

  add_executable(oos_test_nokia_2780_hardware_headless
    tests/hardware_headless_test.cpp)
  oos_nokia_2780_android_target(oos_test_nokia_2780_hardware_headless)
  target_link_libraries(oos_test_nokia_2780_hardware_headless PRIVATE
    oos_nokia_2780_hardware)

  set_target_properties(
    oos_test_nokia_2780_primary_gl
    oos_test_nokia_2780_primary_bufferqueue
    oos_test_nokia_2780_cover_green
    oos_test_nokia_2780_cover_secondary
    oos_test_nokia_2780_wpe_display
    oos_test_nokia_2780_key_input
    oos_test_nokia_2780_network_headless
    oos_test_nokia_2780_modem_headless
    oos_test_nokia_2780_hardware_headless
    PROPERTIES RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/bin/tests/nokia-2780-flip")
endif()
