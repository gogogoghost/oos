cmake_minimum_required(VERSION 3.22)
project(oos_host_runtime_test C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(OOS_SYSTEM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
get_filename_component(OOS_REPO_ROOT "${OOS_SYSTEM_ROOT}/.." ABSOLUTE)
set(WAMR_ROOT_DIR "${OOS_REPO_ROOT}/third_party/wasm-micro-runtime")
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
set(WAMR_BUILD_LIB_PTHREAD 1)
set(WAMR_BUILD_LIB_WASI_THREADS 0)
set(WAMR_BUILD_SHARED_MEMORY 1)
set(WAMR_BUILD_MULTI_MODULE 0)
set(WAMR_BUILD_BULK_MEMORY 1)
set(WAMR_BUILD_REF_TYPES 1)
set(WAMR_BUILD_SIMD 0)
include("${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake")

add_library(oos_wamr_host STATIC ${WAMR_RUNTIME_LIB_SOURCE})
target_include_directories(oos_wamr_host PUBLIC
  "${WAMR_ROOT_DIR}/core/iwasm/include")
target_link_libraries(oos_wamr_host PUBLIC dl m pthread)

include("${OOS_SYSTEM_ROOT}/cmake/QuickJS.cmake")
oos_add_quickjs(oos_quickjs_host "${OOS_REPO_ROOT}/third_party/quickjs")

include("${OOS_SYSTEM_ROOT}/cmake/Sonivox.cmake")
oos_add_sonivox(oos_sonivox_host
  "${OOS_REPO_ROOT}/third_party/sonivox/arm-wt-22k")
include("${OOS_SYSTEM_ROOT}/cmake/FluidLite.cmake")
oos_add_fluidlite("${OOS_REPO_ROOT}/third_party/fluidlite")
include("${OOS_SYSTEM_ROOT}/cmake/FFmpeg.cmake")
oos_add_ffmpeg(oos_ffmpeg_host
  "${OOS_REPO_ROOT}/build/media-codecs/local/install")

add_executable(oos_wasm_runtime_test
  wasm_runtime_test.cpp
  "${OOS_SYSTEM_ROOT}/src/oos/apps/app_manifest.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/apps/app_repository.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/apps/wasm_artifact.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/apps/json.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/input/key_debouncer.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/input/key_input.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/media/audio_decoder.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/media/audio_format.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/media/ffmpeg_decoder.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/media/media_service.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/media/media_source.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/media/midi_decoder.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/media/soundfont_midi_decoder.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/media/miniaudio_decoder.c"
  "${OOS_SYSTEM_ROOT}/src/oos/runtime/application_context.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/runtime/application_scene.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/runtime/canvas_2d.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/runtime/js_app.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/runtime/js_platform_services.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/runtime/module_host.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/runtime/native_ui.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/runtime/native_app_manager.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/runtime/wasm_app.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/resources/font_assets.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/resources/package_assets.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/services/system_service.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/storage/app_storage.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/storage/device_storage.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/storage/filesystem.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/apps/zip_archive.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/storage/sqlite.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/ui/system_ui_settings.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/ui/system_ui_state.cpp"
  "${OOS_SYSTEM_ROOT}/devices/local/src/service_provider.cpp")
target_include_directories(oos_wasm_runtime_test PRIVATE
  "${OOS_SYSTEM_ROOT}/src"
  "${OOS_REPO_ROOT}/third_party/stb"
  "${OOS_REPO_ROOT}/third_party/miniaudio"
  "${OOS_REPO_ROOT}/third_party/tinysoundfont")
include("${OOS_SYSTEM_ROOT}/cmake/EmbeddedSoundFont.cmake")
oos_embed_gm_soundfont(oos_wasm_runtime_test
  "${OOS_REPO_ROOT}/third_party/generaluser-gs/GeneralUser-GS.sf2")
set_source_files_properties(
  "${OOS_SYSTEM_ROOT}/src/oos/media/miniaudio_decoder.c"
  PROPERTIES COMPILE_OPTIONS "-Wno-unused-function;-Wno-unused-variable")
target_link_libraries(oos_wasm_runtime_test PRIVATE
  oos_quickjs_host oos_wamr_host oos_sonivox_host oos_ffmpeg_host fluidlite
  ${CMAKE_DL_LIBS} z)

add_executable(oos_media_core_test
  media_core_test.cpp
  "${OOS_SYSTEM_ROOT}/src/oos/media/audio_decoder.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/media/audio_format.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/media/ffmpeg_decoder.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/media/midi_decoder.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/media/media_source.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/media/soundfont_midi_decoder.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/media/miniaudio_decoder.c")
target_include_directories(oos_media_core_test PRIVATE
  "${OOS_SYSTEM_ROOT}/src"
  "${OOS_REPO_ROOT}/third_party/miniaudio"
  "${OOS_REPO_ROOT}/third_party/tinysoundfont")
set_source_files_properties(
  "${OOS_SYSTEM_ROOT}/src/oos/media/miniaudio_decoder.c"
  PROPERTIES COMPILE_OPTIONS "-Wno-unused-function;-Wno-unused-variable")
oos_embed_gm_soundfont(oos_media_core_test
  "${OOS_REPO_ROOT}/third_party/generaluser-gs/GeneralUser-GS.sf2")
target_link_libraries(oos_media_core_test PRIVATE
  oos_sonivox_host oos_ffmpeg_host fluidlite)

add_executable(oos_app_repository_test
  app_repository_test.cpp
  "${OOS_SYSTEM_ROOT}/src/oos/apps/app_manifest.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/apps/app_repository.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/apps/wasm_artifact.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/apps/json.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/apps/zip_archive.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/resources/package_assets.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/storage/app_storage.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/storage/filesystem.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/storage/sqlite.cpp")
target_include_directories(oos_app_repository_test PRIVATE
  "${OOS_SYSTEM_ROOT}/src")
target_link_libraries(oos_app_repository_test PRIVATE ${CMAKE_DL_LIBS} z)

add_executable(oos_device_storage_test
  device_storage_test.cpp
  "${OOS_SYSTEM_ROOT}/src/oos/storage/device_storage.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/apps/zip_archive.cpp")
target_include_directories(oos_device_storage_test PRIVATE
  "${OOS_SYSTEM_ROOT}/src")
target_compile_options(oos_device_storage_test PRIVATE -UNDEBUG)
target_link_libraries(oos_device_storage_test PRIVATE z)

add_executable(oos_font_assets_test
  font_assets_test.cpp
  "${OOS_SYSTEM_ROOT}/src/oos/resources/font_assets.cpp")
target_include_directories(oos_font_assets_test PRIVATE
  "${OOS_SYSTEM_ROOT}/src")
target_compile_options(oos_font_assets_test PRIVATE -UNDEBUG)

add_executable(oos_system_service_test
  system_service_test.cpp
  "${OOS_SYSTEM_ROOT}/src/oos/apps/app_manifest.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/apps/app_repository.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/apps/wasm_artifact.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/apps/json.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/apps/zip_archive.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/services/system_service.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/storage/app_storage.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/storage/filesystem.cpp"
  "${OOS_SYSTEM_ROOT}/src/oos/storage/sqlite.cpp")
target_include_directories(oos_system_service_test PRIVATE
  "${OOS_SYSTEM_ROOT}/src")
target_compile_options(oos_system_service_test PRIVATE -UNDEBUG)
target_link_libraries(oos_system_service_test PRIVATE ${CMAKE_DL_LIBS} z)
