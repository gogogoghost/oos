set(WAMR_ROOT_DIR "${CMAKE_SOURCE_DIR}/third_party/wasm-micro-runtime")
if(NOT EXISTS "${WAMR_ROOT_DIR}/core/iwasm/include/wasm_export.h")
  message(FATAL_ERROR
    "WAMR is missing. Run scripts/fetch-wamr.sh before configuring OOS.")
endif()

set(WAMR_BUILD_PLATFORM android)
set(WAMR_BUILD_TARGET ARMV7A)
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

add_library(oos_wamr STATIC ${WAMR_RUNTIME_LIB_SOURCE})
target_include_directories(oos_wamr PUBLIC
  "${WAMR_ROOT_DIR}/core/iwasm/include")
target_link_libraries(oos_wamr PUBLIC dl m)
