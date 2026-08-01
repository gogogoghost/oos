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
target_link_libraries(oos_wasm_runtime PUBLIC oos::platform)

target_link_libraries(oos PRIVATE
  oos::platform
  oos_wasm_runtime
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

  add_executable(oos_test_ui_backends
    "${CMAKE_SOURCE_DIR}/tests/host/ui_backend_test.cpp")
  target_compile_options(oos_test_ui_backends PRIVATE -UNDEBUG)
  target_link_libraries(oos_test_ui_backends PRIVATE
    oos_sdk_ui oos_launcher_app oos_systemui_app)
  add_test(NAME ui-backends COMMAND oos_test_ui_backends)
endif()
