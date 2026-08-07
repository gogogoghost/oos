find_package(PNG REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
find_package(OpenGL REQUIRED COMPONENTS GLES2)

set(OOS_LOCAL_KEYMAP_SOURCE
  "${CMAKE_CURRENT_SOURCE_DIR}/config/keymap.conf")
set(OOS_LOCAL_KEYMAP_BUILD
  "${CMAKE_BINARY_DIR}/config/local-keymap.conf")
configure_file("${OOS_LOCAL_KEYMAP_SOURCE}" "${OOS_LOCAL_KEYMAP_BUILD}" COPYONLY)

add_library(oos_local_platform STATIC
  src/device.cpp
  src/local_display.cpp
  src/local_key_input.cpp
  src/service_provider.cpp)
target_include_directories(oos_local_platform PUBLIC
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(oos_local_platform PRIVATE
  OOS_LOCAL_DEFAULT_KEYMAP="${OOS_LOCAL_KEYMAP_BUILD}")
target_link_libraries(oos_local_platform PUBLIC
  oos_device_api
  oos_gles_executor
  SDL3::SDL3
  OpenGL::GLES2)
add_library(oos::platform ALIAS oos_local_platform)
target_link_libraries(oos_app_runtime PUBLIC oos::platform)

target_link_libraries(oos PRIVATE
  oos::platform
  oos_app_runtime
  PNG::PNG)

if(OOS_BUILD_DEVICE_TESTS)
  enable_testing()
  add_executable(oos_test_local_keymap tests/keymap_test.cpp
    src/local_key_input.cpp)
  target_include_directories(oos_test_local_keymap PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(oos_test_local_keymap PRIVATE SDL3::SDL3)
  add_test(NAME local-keymap COMMAND oos_test_local_keymap
    "${OOS_LOCAL_KEYMAP_BUILD}")

  add_executable(oos_test_key_debouncer
    "${CMAKE_SOURCE_DIR}/tests/host/key_debouncer_test.cpp")
  target_compile_options(oos_test_key_debouncer PRIVATE -UNDEBUG)
  target_link_libraries(oos_test_key_debouncer PRIVATE oos_input)
  add_test(NAME input-debounce COMMAND oos_test_key_debouncer)

  add_executable(oos_test_supplicant_text
    "${CMAKE_SOURCE_DIR}/tests/host/supplicant_text_test.cpp"
    "${CMAKE_SOURCE_DIR}/src/oos/network/supplicant_text.cpp")
  target_compile_options(oos_test_supplicant_text PRIVATE -UNDEBUG)
  target_include_directories(oos_test_supplicant_text PRIVATE
    "${CMAKE_SOURCE_DIR}/src")
  add_test(NAME supplicant-text COMMAND oos_test_supplicant_text)

  add_executable(oos_test_ui_backends
    "${CMAKE_SOURCE_DIR}/tests/host/ui_backend_test.cpp"
    "${OOS_REPO_ROOT}/apps/launcher/src/launcher.cpp"
    "${OOS_REPO_ROOT}/apps/settings/src/settings.cpp"
    "${OOS_REPO_ROOT}/apps/systemui/src/system_ui.cpp")
  set(OOS_TEST_LAUNCHER_LOGO
      "${OOS_REPO_ROOT}/apps/launcher/assets/oos-logo-32-bgra.bin")
  file(READ "${OOS_TEST_LAUNCHER_LOGO}" OOS_LAUNCHER_LOGO_HEX HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1,"
    OOS_LAUNCHER_LOGO_BYTES "${OOS_LAUNCHER_LOGO_HEX}")
  configure_file(
    "${OOS_REPO_ROOT}/apps/launcher/src/embedded_logo.cpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/test_embedded_logo.cpp" @ONLY)
  target_sources(oos_test_ui_backends PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/test_embedded_logo.cpp")
  unset(OOS_LAUNCHER_LOGO_HEX)
  unset(OOS_LAUNCHER_LOGO_BYTES)
  target_include_directories(oos_test_ui_backends PRIVATE
    "${OOS_REPO_ROOT}/apps/launcher/include"
    "${OOS_REPO_ROOT}/apps/settings/include"
    "${OOS_REPO_ROOT}/apps/systemui/include")
  target_compile_options(oos_test_ui_backends PRIVATE -UNDEBUG)
  target_link_libraries(oos_test_ui_backends PRIVATE
    oos_sdk_ui oos_system_settings oos_app_sessions oos_app_runtime
    oos::platform)
  add_test(NAME ui-backends COMMAND oos_test_ui_backends)

  add_executable(oos_test_js_runtime
    "${CMAKE_SOURCE_DIR}/tests/host/js_runtime_test.cpp")
  target_compile_options(oos_test_js_runtime PRIVATE -UNDEBUG)
  target_link_libraries(oos_test_js_runtime PRIVATE oos_app_runtime)
  add_test(NAME js-runtime COMMAND oos_test_js_runtime)
endif()
