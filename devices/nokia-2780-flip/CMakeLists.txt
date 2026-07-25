set(NOKIA_2780_SYSTEM_LIB_DIR "${SYSTEM_DIR}/lib")
set(NOKIA_2780_HWC_DIR
    "${THIRD_PARTY_DIR}/gecko-b2g/widget/gonk/hwchal/android_10")

add_executable(nokia_2780_primary_gl_animation
  src/primary_gl_animation_poc.cpp
  "${NOKIA_2780_HWC_DIR}/HWC2.cpp"
  "${NOKIA_2780_HWC_DIR}/ComposerHal.cpp")
target_compile_definitions(nokia_2780_primary_gl_animation PRIVATE
  ANDROID_VERSION=29
  _LIBCPP_ABI_NAMESPACE=__1)
target_compile_options(nokia_2780_primary_gl_animation PRIVATE
  -include "${CMAKE_CURRENT_SOURCE_DIR}/include/poc_compat.h"
  -fno-rtti)
target_include_directories(nokia_2780_primary_gl_animation PRIVATE
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
  "${THIRD_PARTY_DIR}/aosp-system-libhidl/base/include"
  "${THIRD_PARTY_DIR}/aosp-system-libhidl/transport/include"
  "${THIRD_PARTY_DIR}/aosp-system-libhidl/transport/token/1.0/utils/include"
  "${THIRD_PARTY_DIR}/aosp-system-libfmq/include"
  "${THIRD_PARTY_DIR}/aosp-hardware-libhardware/include"
  "${NOKIA_2780_HWC_DIR}")
target_link_options(nokia_2780_primary_gl_animation PRIVATE
  -nostdlib++
  "-Wl,-rpath-link,${NOKIA_2780_SYSTEM_LIB_DIR}")
target_link_libraries(nokia_2780_primary_gl_animation PRIVATE
  "${NOKIA_2780_SYSTEM_LIB_DIR}/libc++.so"
  EGL GLESv2 android
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

add_executable(nokia_2780_cover_green src/cover_green_poc.c)

set_target_properties(nokia_2780_primary_gl_animation nokia_2780_cover_green
  PROPERTIES RUNTIME_OUTPUT_DIRECTORY
  "${CMAKE_BINARY_DIR}/bin/nokia-2780-flip")
