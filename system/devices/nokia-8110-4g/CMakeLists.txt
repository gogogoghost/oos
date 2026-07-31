set(NOKIA_8110_SYSTEM_LIB_DIR "${SYSTEM_DIR}/lib")
set(NOKIA_8110_FRAMEWORKS_NATIVE_DIR
    "${THIRD_PARTY_DIR}/aosp-frameworks-native-android6")
set(NOKIA_8110_SYSTEM_CORE_DIR
    "${THIRD_PARTY_DIR}/aosp-system-core-android6")
set(NOKIA_8110_LIBHARDWARE_DIR
    "${THIRD_PARTY_DIR}/aosp-hardware-libhardware-android6")
set(NOKIA_8110_LIBPNG_DIR "${THIRD_PARTY_DIR}/aosp-libpng-android6")
set(NOKIA_8110_SYSTEM_MEDIA_DIR
    "${THIRD_PARTY_DIR}/aosp-system-media-android6")
set(NOKIA_8110_BIONIC_DIR "${THIRD_PARTY_DIR}/aosp-bionic-android6")
set(NOKIA_8110_WPE_SYSROOT
    "${OOS_REPO_ROOT}/build/wpe-sysroot/nokia-8110-4g")
set(NOKIA_8110_ANDROID_INCLUDES
  "${NOKIA_8110_FRAMEWORKS_NATIVE_DIR}/include"
  "${NOKIA_8110_SYSTEM_CORE_DIR}/include"
  "${NOKIA_8110_LIBHARDWARE_DIR}/include")

set(NOKIA_8110_WPE_INCLUDES
  "${NOKIA_8110_WPE_SYSROOT}/include/wpe-webkit-2.0"
  "${NOKIA_8110_WPE_SYSROOT}/include/wpe-webkit-2.0/wpe-platform"
  "${NOKIA_8110_WPE_SYSROOT}/include/wpe-1.0"
  "${NOKIA_8110_WPE_SYSROOT}/include"
  "${NOKIA_8110_WPE_SYSROOT}/include/glib-2.0"
  "${NOKIA_8110_WPE_SYSROOT}/include/gio-unix-2.0"
  "${NOKIA_8110_WPE_SYSROOT}/include/libsoup-3.0"
  "${NOKIA_8110_WPE_SYSROOT}/lib/glib-2.0/include")

if(NOT EXISTS "${NOKIA_8110_SYSTEM_LIB_DIR}/hw/hwcomposer.msm8909.so")
  message(FATAL_ERROR
    "SYSTEM_DIR is not a Nokia 8110 4G system tree: missing hwcomposer.msm8909.so")
endif()
if(NOT EXISTS
   "${NOKIA_8110_FRAMEWORKS_NATIVE_DIR}/include/ui/FramebufferNativeWindow.h")
  message(FATAL_ERROR
    "Android 6 AOSP headers are missing. See third_party/README.md.")
endif()
if(NOT EXISTS "${NOKIA_8110_LIBPNG_DIR}/png.h")
  message(FATAL_ERROR
    "Android 6 libpng headers are missing. See third_party/README.md.")
endif()
if(NOT EXISTS "${NOKIA_8110_SYSTEM_MEDIA_DIR}/audio/include/system/audio.h")
  message(FATAL_ERROR
    "Android 6 system/media headers are missing. See third_party/README.md.")
endif()

set(NOKIA_8110_GRAPHICS_LIBRARIES
  "${NOKIA_8110_SYSTEM_LIB_DIR}/libEGL.so"
  "${NOKIA_8110_SYSTEM_LIB_DIR}/libGLESv2.so"
  "${NOKIA_8110_SYSTEM_LIB_DIR}/libui.so"
  "${NOKIA_8110_SYSTEM_LIB_DIR}/libutils.so"
  "${NOKIA_8110_SYSTEM_LIB_DIR}/libhardware.so"
  "${NOKIA_8110_SYSTEM_LIB_DIR}/libsync.so"
  "${NOKIA_8110_SYSTEM_LIB_DIR}/liblog.so"
  "${NOKIA_8110_SYSTEM_LIB_DIR}/libcutils.so"
  "${NOKIA_8110_SYSTEM_LIB_DIR}/libc++.so")

function(oos_nokia_8110_android_target target)
  target_compile_definitions(${target} PRIVATE
    ANDROID_VERSION=23
    _LIBCPP_ABI_NAMESPACE=__1)
  target_include_directories(${target} PRIVATE ${NOKIA_8110_ANDROID_INCLUDES})
  target_link_options(${target} PRIVATE
    -nostdlib++
    -Wl,--allow-shlib-undefined
    "-Wl,-rpath-link,${NOKIA_8110_SYSTEM_LIB_DIR}")
endfunction()

# WPE's Android port requires AHardwareBuffer (API 26). Android 6 uses the
# equivalent gralloc0 native-handle path through this device-only ABI shim.
add_library(oos_android23_hardware_buffer SHARED
  src/android23_hardware_buffer.c)
oos_nokia_8110_android_target(oos_android23_hardware_buffer)
set_target_properties(oos_android23_hardware_buffer PROPERTIES
  OUTPUT_NAME "oos-android23-buffer")
target_link_libraries(oos_android23_hardware_buffer PRIVATE
  "${NOKIA_8110_SYSTEM_LIB_DIR}/libhardware.so"
  "${NOKIA_8110_SYSTEM_LIB_DIR}/libcutils.so"
  "${NOKIA_8110_SYSTEM_LIB_DIR}/liblog.so")

add_library(oos_nokia_8110_gles_display STATIC src/primary_gles_display.cpp)
oos_nokia_8110_android_target(oos_nokia_8110_gles_display)
target_include_directories(oos_nokia_8110_gles_display PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(oos_nokia_8110_gles_display PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
  "${NOKIA_8110_SYSTEM_CORE_DIR}/libsync/include")
target_link_libraries(oos_nokia_8110_gles_display PUBLIC
  oos_gles_executor
  "${NOKIA_8110_SYSTEM_LIB_DIR}/libEGL.so"
  "${NOKIA_8110_SYSTEM_LIB_DIR}/libGLESv2.so"
  "${NOKIA_8110_SYSTEM_LIB_DIR}/libhardware.so"
  "${NOKIA_8110_SYSTEM_LIB_DIR}/libsync.so"
  "${NOKIA_8110_SYSTEM_LIB_DIR}/liblog.so"
  "${NOKIA_8110_SYSTEM_LIB_DIR}/libcutils.so"
  oos_android23_hardware_buffer
  "${NOKIA_8110_SYSTEM_LIB_DIR}/libc++.so")

if(EXISTS "${NOKIA_8110_WPE_SYSROOT}/lib/libWPEBackend-android.so")
  add_library(oos_wpe_surface_host STATIC
    "${CMAKE_SOURCE_DIR}/src/oos/web/wpe_surface_host.cpp")
  target_include_directories(oos_wpe_surface_host PUBLIC
    "${CMAKE_SOURCE_DIR}/src")
  target_include_directories(oos_wpe_surface_host PRIVATE
    ${NOKIA_8110_WPE_INCLUDES})
  target_link_libraries(oos_wpe_surface_host PUBLIC
    "${NOKIA_8110_WPE_SYSROOT}/lib/libWPEBackend-android.so"
    "${NOKIA_8110_WPE_SYSROOT}/lib/libwpe-1.0.so")
  add_library(oos::web ALIAS oos_wpe_surface_host)

  add_executable(oos-wpe
    "${CMAKE_SOURCE_DIR}/src/oos/web/wpe_app_runner.cpp"
    "${CMAKE_SOURCE_DIR}/src/oos/web/transport_surface_sink.cpp"
    "${CMAKE_SOURCE_DIR}/src/oos/web/kaios_api_bridge.cpp"
    "${CMAKE_SOURCE_DIR}/src/oos/web/zip_app_source.cpp"
    "${CMAKE_SOURCE_DIR}/src/oos/web/wpe_app_profile.cpp"
    "${CMAKE_SOURCE_DIR}/src/oos/web/wpe_key_input.cpp")
  target_include_directories(oos-wpe PRIVATE ${NOKIA_8110_WPE_INCLUDES})
  target_link_libraries(oos-wpe PRIVATE
    oos_wpe_surface_host
    oos_surface_transport
    oos_app_model
    "${NOKIA_8110_WPE_SYSROOT}/lib/libWPEWebKit-2.0.so"
    "${NOKIA_8110_WPE_SYSROOT}/lib/libwpe-1.0.so"
    "${NOKIA_8110_WPE_SYSROOT}/lib/libgio-2.0.so"
    "${NOKIA_8110_WPE_SYSROOT}/lib/libglib-2.0.so"
    "${NOKIA_8110_WPE_SYSROOT}/lib/libgobject-2.0.so"
    "${NOKIA_8110_WPE_SYSROOT}/lib/libsoup-3.0.so"
    oos_android23_hardware_buffer
    "${NOKIA_8110_SYSTEM_LIB_DIR}/libc++.so")
  target_link_options(oos-wpe PRIVATE
    -Wl,--allow-shlib-undefined
    "-Wl,-rpath-link,${NOKIA_8110_SYSTEM_LIB_DIR}")
  set_target_properties(oos-wpe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

add_library(oos_nokia_8110_device STATIC
  src/device.cpp
  "${CMAKE_SOURCE_DIR}/src/oos/device/android_service_provider.cpp")
oos_nokia_8110_android_target(oos_nokia_8110_device)
target_include_directories(oos_nokia_8110_device PUBLIC
  "${CMAKE_SOURCE_DIR}/src")
target_include_directories(oos_nokia_8110_device PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(oos_nokia_8110_device PUBLIC
  oos_device_api
  oos_input
  oos_nokia_8110_gles_display)
add_library(oos::platform ALIAS oos_nokia_8110_device)
target_link_libraries(oos_wasm_runtime PUBLIC oos::platform)

oos_nokia_8110_android_target(oos)
target_include_directories(oos PRIVATE
  "${NOKIA_8110_LIBPNG_DIR}")
target_link_libraries(oos PRIVATE
  oos::platform
  oos_wasm_runtime
  oos_surface_transport
  "${NOKIA_8110_SYSTEM_LIB_DIR}/libpng.so"
  "${NOKIA_8110_SYSTEM_LIB_DIR}/libc++.so")

add_library(oos_nokia_8110_network STATIC
  "${CMAKE_SOURCE_DIR}/src/oos/network/wifi_manager.cpp"
  "${CMAKE_SOURCE_DIR}/src/oos/network/ip_manager.cpp"
  "${CMAKE_SOURCE_DIR}/src/oos/network/bluetooth_manager.cpp")
oos_nokia_8110_android_target(oos_nokia_8110_network)
target_include_directories(oos_nokia_8110_network PUBLIC
  "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(oos_nokia_8110_network PUBLIC
  "${NOKIA_8110_SYSTEM_LIB_DIR}/libc++.so"
  "${NOKIA_8110_SYSTEM_LIB_DIR}/libcutils.so"
  "${NOKIA_8110_SYSTEM_LIB_DIR}/libnetutils.so"
  "${NOKIA_8110_SYSTEM_LIB_DIR}/liblog.so")
add_library(oos::network ALIAS oos_nokia_8110_network)

add_library(oos_nokia_8110_modem STATIC src/modem_manager.cpp)
oos_nokia_8110_android_target(oos_nokia_8110_modem)
target_include_directories(oos_nokia_8110_modem PUBLIC
  "${CMAKE_SOURCE_DIR}/src")
target_include_directories(oos_nokia_8110_modem PRIVATE
  "${THIRD_PARTY_DIR}/aosp-hardware-ril-android6/include")
target_link_libraries(oos_nokia_8110_modem PUBLIC
  "${NOKIA_8110_SYSTEM_LIB_DIR}/libc++.so")
add_library(oos::modem ALIAS oos_nokia_8110_modem)

add_library(oos_nokia_8110_basic_hardware STATIC
  src/audio_manager.cpp
  src/camera_manager.cpp
  src/codec_manager.cpp
  src/permission_controller.cpp
  src/power_manager.cpp
  src/vibrator_manager.cpp)
oos_nokia_8110_android_target(oos_nokia_8110_basic_hardware)
target_compile_options(oos_nokia_8110_basic_hardware PRIVATE -fno-rtti)
target_include_directories(oos_nokia_8110_basic_hardware PUBLIC
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(oos_nokia_8110_basic_hardware PRIVATE
  "${NOKIA_8110_BIONIC_DIR}/libc/kernel/uapi"
  "${NOKIA_8110_SYSTEM_MEDIA_DIR}/camera/include"
  "${NOKIA_8110_SYSTEM_CORE_DIR}/libsuspend/include")
target_link_libraries(oos_nokia_8110_basic_hardware PUBLIC
  "${NOKIA_8110_SYSTEM_LIB_DIR}/libOpenSLES.so"
  "${NOKIA_8110_SYSTEM_LIB_DIR}/libsuspend.so"
  "${NOKIA_8110_SYSTEM_LIB_DIR}/libhardware.so"
  "${NOKIA_8110_SYSTEM_LIB_DIR}/libbinder.so"
  "${NOKIA_8110_SYSTEM_LIB_DIR}/libutils.so"
  "${NOKIA_8110_SYSTEM_LIB_DIR}/libcutils.so"
  "${NOKIA_8110_SYSTEM_LIB_DIR}/liblog.so"
  "${NOKIA_8110_SYSTEM_LIB_DIR}/libc++.so")
add_library(oos::hardware ALIAS oos_nokia_8110_basic_hardware)

target_link_libraries(oos_nokia_8110_device PUBLIC
  oos::hardware oos::network oos::modem)

if(OOS_BUILD_DEVICE_TESTS)
  add_executable(oos_test_nokia_8110_surface_transport_key
    "${CMAKE_SOURCE_DIR}/tests/web/surface_transport_key_test.cpp")
  oos_nokia_8110_android_target(oos_test_nokia_8110_surface_transport_key)
  target_link_libraries(oos_test_nokia_8110_surface_transport_key PRIVATE
    oos_surface_transport oos_android23_hardware_buffer
    "${NOKIA_8110_SYSTEM_LIB_DIR}/libc++.so")

  if(TARGET oos_wpe_surface_host AND
     EXISTS "${NOKIA_8110_WPE_SYSROOT}/lib/libWPEWebKit-2.0.so")
    add_executable(oos_test_nokia_8110_4g_wpe_producer
      "${CMAKE_SOURCE_DIR}/tests/web/wpe_hello_producer.cpp"
      "${CMAKE_SOURCE_DIR}/src/oos/web/transport_surface_sink.cpp"
      "${CMAKE_SOURCE_DIR}/src/oos/web/wpe_app_profile.cpp"
      "${CMAKE_SOURCE_DIR}/src/oos/web/zip_app_source.cpp")
    oos_nokia_8110_android_target(oos_test_nokia_8110_4g_wpe_producer)
    target_include_directories(oos_test_nokia_8110_4g_wpe_producer PRIVATE
      ${NOKIA_8110_WPE_INCLUDES})
    target_link_libraries(oos_test_nokia_8110_4g_wpe_producer PRIVATE
      oos_wpe_surface_host
      oos_surface_transport
      oos_app_model
      "${NOKIA_8110_WPE_SYSROOT}/lib/libWPEWebKit-2.0.so"
      "${NOKIA_8110_WPE_SYSROOT}/lib/libglib-2.0.so"
      "${NOKIA_8110_WPE_SYSROOT}/lib/libgobject-2.0.so"
      "${NOKIA_8110_WPE_SYSROOT}/lib/libsoup-3.0.so"
      oos_android23_hardware_buffer
      "${NOKIA_8110_SYSTEM_LIB_DIR}/libc++.so")

    add_executable(oos_test_nokia_8110_4g_wpe_host
      "${CMAKE_SOURCE_DIR}/tests/web/wpe_surface_host_test.cpp")
    oos_nokia_8110_android_target(oos_test_nokia_8110_4g_wpe_host)
    target_link_libraries(oos_test_nokia_8110_4g_wpe_host PRIVATE
      oos::platform
      oos_compositor
      oos_surface_transport
      oos_android23_hardware_buffer
      "${NOKIA_8110_SYSTEM_LIB_DIR}/libc++.so")

    set_target_properties(
      oos_test_nokia_8110_4g_wpe_producer
      oos_test_nokia_8110_4g_wpe_host
      PROPERTIES RUNTIME_OUTPUT_DIRECTORY
      "${CMAKE_BINARY_DIR}/bin/tests/nokia-8110-4g")
  endif()

  add_executable(oos_test_nokia_8110_device_contract
    "${CMAKE_SOURCE_DIR}/tests/device/device_contract_test.cpp")
  oos_nokia_8110_android_target(oos_test_nokia_8110_device_contract)
  target_link_libraries(oos_test_nokia_8110_device_contract PRIVATE
    oos::platform)

  add_executable(oos_test_nokia_8110_service_contract
    "${CMAKE_SOURCE_DIR}/tests/device/service_contract_test.cpp")
  oos_nokia_8110_android_target(oos_test_nokia_8110_service_contract)
  target_link_libraries(oos_test_nokia_8110_service_contract PRIVATE
    oos::platform oos::hardware oos::network oos::modem)

  add_executable(oos_probe_nokia_8110_ril tests/ril_socket_probe.cpp)
  oos_nokia_8110_android_target(oos_probe_nokia_8110_ril)
  target_include_directories(oos_probe_nokia_8110_ril PRIVATE
    "${THIRD_PARTY_DIR}/aosp-hardware-ril-android6/include")
  target_link_libraries(oos_probe_nokia_8110_ril PRIVATE
    "${NOKIA_8110_SYSTEM_LIB_DIR}/libc++.so")

  add_executable(oos_test_nokia_8110_modem
    "${CMAKE_SOURCE_DIR}/tests/device/modem_headless_test.cpp")
  oos_nokia_8110_android_target(oos_test_nokia_8110_modem)
  target_link_libraries(oos_test_nokia_8110_modem PRIVATE
    oos::platform oos::modem)

  add_executable(oos_probe_nokia_8110_camera tests/camera_hal_probe.cpp)
  oos_nokia_8110_android_target(oos_probe_nokia_8110_camera)
  target_include_directories(oos_probe_nokia_8110_camera PRIVATE
    "${NOKIA_8110_SYSTEM_MEDIA_DIR}/camera/include")
  target_link_libraries(oos_probe_nokia_8110_camera PRIVATE
    "${NOKIA_8110_SYSTEM_LIB_DIR}/libhardware.so"
    "${NOKIA_8110_SYSTEM_LIB_DIR}/liblog.so"
    "${NOKIA_8110_SYSTEM_LIB_DIR}/libcutils.so"
    "${NOKIA_8110_SYSTEM_LIB_DIR}/libc++.so")

  add_executable(oos_test_nokia_8110_basic_hardware
    tests/basic_hardware_test.cpp)
  oos_nokia_8110_android_target(oos_test_nokia_8110_basic_hardware)
  target_link_libraries(oos_test_nokia_8110_basic_hardware PRIVATE
    oos_nokia_8110_basic_hardware)

  add_executable(oos_test_nokia_8110_key_input
    tests/key_input_headless_test.cpp)
  oos_nokia_8110_android_target(oos_test_nokia_8110_key_input)
  target_link_libraries(oos_test_nokia_8110_key_input PRIVATE
    oos_input
    "${NOKIA_8110_SYSTEM_LIB_DIR}/libc++.so")

  add_executable(oos_test_nokia_8110_network_headless
    "${CMAKE_SOURCE_DIR}/tests/device/network_headless_test.cpp")
  oos_nokia_8110_android_target(oos_test_nokia_8110_network_headless)
  target_link_libraries(oos_test_nokia_8110_network_headless PRIVATE
    oos::platform oos::network)

  add_executable(oos_test_nokia_8110_display_power
    tests/display_power_test.cpp)
  oos_nokia_8110_android_target(oos_test_nokia_8110_display_power)
  target_link_libraries(oos_test_nokia_8110_display_power PRIVATE
    "${NOKIA_8110_SYSTEM_LIB_DIR}/libhardware.so"
    "${NOKIA_8110_SYSTEM_LIB_DIR}/liblog.so"
    "${NOKIA_8110_SYSTEM_LIB_DIR}/libcutils.so"
    "${NOKIA_8110_SYSTEM_LIB_DIR}/libc++.so")

  add_executable(oos_probe_nokia_8110_graphics tests/graphics_stack_probe.c)
  oos_nokia_8110_android_target(oos_probe_nokia_8110_graphics)
  target_link_libraries(oos_probe_nokia_8110_graphics PRIVATE
    "${NOKIA_8110_SYSTEM_LIB_DIR}/libhardware.so"
    "${NOKIA_8110_SYSTEM_LIB_DIR}/liblog.so"
    "${NOKIA_8110_SYSTEM_LIB_DIR}/libcutils.so"
    "${NOKIA_8110_SYSTEM_LIB_DIR}/libEGL.so"
    "${NOKIA_8110_SYSTEM_LIB_DIR}/libGLESv2.so")

  add_executable(oos_test_nokia_8110_framebuffer_gles
    tests/framebuffer_gles_test.cpp)
  oos_nokia_8110_android_target(oos_test_nokia_8110_framebuffer_gles)
  target_link_libraries(oos_test_nokia_8110_framebuffer_gles PRIVATE
    ${NOKIA_8110_GRAPHICS_LIBRARIES})

  add_executable(oos_test_nokia_8110_hwc1_gles tests/hwc1_gles_test.cpp)
  oos_nokia_8110_android_target(oos_test_nokia_8110_hwc1_gles)
  target_include_directories(oos_test_nokia_8110_hwc1_gles PRIVATE
    "${NOKIA_8110_SYSTEM_CORE_DIR}/libsync/include")
  target_link_libraries(oos_test_nokia_8110_hwc1_gles PRIVATE
    ${NOKIA_8110_GRAPHICS_LIBRARIES})

  set_target_properties(oos_probe_nokia_8110_graphics PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/bin/tests/nokia-8110-4g")
  set_target_properties(oos_test_nokia_8110_device_contract PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/bin/tests/nokia-8110-4g")
  set_target_properties(oos_test_nokia_8110_service_contract PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/bin/tests/nokia-8110-4g")
  set_target_properties(oos_test_nokia_8110_display_power PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/bin/tests/nokia-8110-4g")
  set_target_properties(oos_test_nokia_8110_framebuffer_gles PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/bin/tests/nokia-8110-4g")
  set_target_properties(oos_test_nokia_8110_hwc1_gles PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/bin/tests/nokia-8110-4g")
  set_target_properties(oos_test_nokia_8110_network_headless PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/bin/tests/nokia-8110-4g")
  set_target_properties(oos_test_nokia_8110_key_input PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/bin/tests/nokia-8110-4g")
  set_target_properties(oos_test_nokia_8110_basic_hardware PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/bin/tests/nokia-8110-4g")
  set_target_properties(oos_probe_nokia_8110_camera PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/bin/tests/nokia-8110-4g")
  set_target_properties(oos_probe_nokia_8110_ril PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/bin/tests/nokia-8110-4g")
  set_target_properties(oos_test_nokia_8110_modem PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/bin/tests/nokia-8110-4g")
endif()
