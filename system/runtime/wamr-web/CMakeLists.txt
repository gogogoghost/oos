cmake_minimum_required(VERSION 3.22)
project(oos_wamr_web_process C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(OOS_REPO_ROOT "" CACHE PATH "OOS repository root")
set(OOS_WPE_PREFIX "" CACHE PATH "Installed WPE prefix")
set(OOS_WAMR_WEB_PLATFORM "linux" CACHE STRING "WAMR platform")
set(OOS_WAMR_WEB_TARGET "X86_64" CACHE STRING "WAMR target")
option(OOS_WAMR_WEB_JIT "Use the WAMR LLVM JIT for WebAssembly" ON)

if(NOT EXISTS "${OOS_REPO_ROOT}/third_party/wasm-micro-runtime/core/iwasm/include/wasm_c_api.h")
  message(FATAL_ERROR "OOS_REPO_ROOT does not contain the pinned WAMR checkout")
endif()
if(NOT EXISTS "${OOS_WPE_PREFIX}/include/wpe-webkit-2.0/wpe/WebKitWebProcessExtension.h")
  message(FATAL_ERROR "OOS_WPE_PREFIX does not contain WPE WebProcess headers")
endif()

set(WAMR_ROOT_DIR "${OOS_REPO_ROOT}/third_party/wasm-micro-runtime")
set(WAMR_BUILD_PLATFORM "${OOS_WAMR_WEB_PLATFORM}")
set(WAMR_BUILD_TARGET "${OOS_WAMR_WEB_TARGET}")
set(WAMR_BUILD_INTERP 1)
if(OOS_WAMR_WEB_JIT)
  if(NOT OOS_WAMR_WEB_PLATFORM STREQUAL "linux" OR
     NOT OOS_WAMR_WEB_TARGET STREQUAL "X86_64")
    message(FATAL_ERROR "WAMR Web LLVM JIT is currently supported only by the local x86_64 profile")
  endif()
  set(WAMR_BUILD_FAST_INTERP 0)
  set(WAMR_BUILD_JIT 1)
else()
  set(WAMR_BUILD_FAST_INTERP 1)
  set(WAMR_BUILD_JIT 0)
endif()
set(WAMR_BUILD_FAST_JIT 0)
set(WAMR_BUILD_AOT 1)
set(WAMR_BUILD_LIBC_BUILTIN 0)
set(WAMR_BUILD_LIBC_WASI 0)
set(WAMR_BUILD_LIB_PTHREAD 0)
set(WAMR_BUILD_LIB_WASI_THREADS 0)
set(WAMR_BUILD_MULTI_MODULE 0)
set(WAMR_BUILD_BULK_MEMORY 1)
set(WAMR_BUILD_REF_TYPES 1)
set(WAMR_BUILD_SIMD 0)
set(WAMR_BUILD_ALLOC_WITH_USAGE 1)
set(WAMR_BUILD_ALLOC_WITH_USER_DATA 1)

include("${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake")

add_library(oos-wamr-web-process MODULE
  src/web_process_extension.cpp
  ${WAMR_RUNTIME_LIB_SOURCE})
target_include_directories(oos-wamr-web-process PRIVATE
  "${WAMR_ROOT_DIR}/core/iwasm/include"
  "${OOS_WPE_PREFIX}/include/wpe-webkit-2.0"
  "${OOS_WPE_PREFIX}/include/wpe-1.0")
if(OOS_WAMR_WEB_PLATFORM STREQUAL "linux")
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0 gobject-2.0)
  pkg_check_modules(SOUP REQUIRED libsoup-3.0)
  target_include_directories(oos-wamr-web-process PRIVATE
    ${SOUP_INCLUDE_DIRS})
  target_compile_options(oos-wamr-web-process PRIVATE
    ${SOUP_CFLAGS_OTHER})
  target_link_libraries(oos-wamr-web-process PRIVATE
    "${OOS_WPE_PREFIX}/lib/libWPEWebKit-2.0.so"
    PkgConfig::GLIB dl m pthread)
else()
  foreach(required_path
      include/glib-2.0
      lib/glib-2.0/include
      include/libsoup-3.0
      lib/libglib-2.0.so
      lib/libgobject-2.0.so)
    if(NOT EXISTS "${OOS_WPE_PREFIX}/${required_path}")
      message(FATAL_ERROR "WPE target sysroot is missing ${required_path}")
    endif()
  endforeach()
  target_include_directories(oos-wamr-web-process PRIVATE
    "${OOS_WPE_PREFIX}/include/glib-2.0"
    "${OOS_WPE_PREFIX}/lib/glib-2.0/include"
    "${OOS_WPE_PREFIX}/include/libsoup-3.0")
  target_link_libraries(oos-wamr-web-process PRIVATE
    "${OOS_WPE_PREFIX}/lib/libWPEWebKit-2.0.so"
    "${OOS_WPE_PREFIX}/lib/libgobject-2.0.so"
    "${OOS_WPE_PREFIX}/lib/libglib-2.0.so"
    dl m)
  target_link_options(oos-wamr-web-process PRIVATE
    -Wl,--allow-shlib-undefined
    "-Wl,-rpath-link,${OOS_WPE_PREFIX}/lib")
endif()
if(OOS_WAMR_WEB_JIT)
  target_link_libraries(oos-wamr-web-process PRIVATE ${LLVM_AVAILABLE_LIBS})
endif()
set_target_properties(oos-wamr-web-process PROPERTIES
  PREFIX "lib"
  BUILD_RPATH "${OOS_WPE_PREFIX}/lib"
  INSTALL_RPATH "/opt/oos/lib")

install(TARGETS oos-wamr-web-process
  LIBRARY DESTINATION lib/oos/web-process-extensions)
