cmake_minimum_required(VERSION 3.22)
project(oos_host_runtime_test C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(OOS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(WAMR_ROOT_DIR "${OOS_ROOT}/third_party/wasm-micro-runtime")
if(NOT EXISTS "${WAMR_ROOT_DIR}/core/iwasm/include/wasm_export.h")
  message(FATAL_ERROR "Run scripts/fetch-wamr.sh first")
endif()

set(WAMR_BUILD_PLATFORM linux)
set(WAMR_BUILD_TARGET X86_64)
set(WAMR_BUILD_INTERP 1)
set(WAMR_BUILD_FAST_INTERP 1)
set(WAMR_BUILD_AOT 1)
set(WAMR_BUILD_FAST_JIT 0)
set(WAMR_BUILD_JIT 0)
set(WAMR_BUILD_LIBC_BUILTIN 0)
set(WAMR_BUILD_LIBC_WASI 0)
set(WAMR_BUILD_LIB_PTHREAD 0)
set(WAMR_BUILD_LIB_WASI_THREADS 0)
set(WAMR_BUILD_MULTI_MODULE 0)
set(WAMR_BUILD_BULK_MEMORY 1)
set(WAMR_BUILD_REF_TYPES 1)
set(WAMR_BUILD_SIMD 0)
include("${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake")

add_library(oos_wamr_host STATIC ${WAMR_RUNTIME_LIB_SOURCE})
target_include_directories(oos_wamr_host PUBLIC
  "${WAMR_ROOT_DIR}/core/iwasm/include")
target_link_libraries(oos_wamr_host PUBLIC dl m pthread)

add_executable(oos_wasm_runtime_test
  wasm_runtime_test.cpp
  "${OOS_ROOT}/core/src/native_app_manager.cpp"
  "${OOS_ROOT}/core/src/wasm_app.cpp")
target_include_directories(oos_wasm_runtime_test PRIVATE
  "${OOS_ROOT}/core/include"
  "${OOS_ROOT}/sdk/include")
target_link_libraries(oos_wasm_runtime_test PRIVATE oos_wamr_host)
