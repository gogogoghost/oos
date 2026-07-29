find_package(PNG REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
find_package(OpenGL REQUIRED COMPONENTS GLES2)

option(OOS_LOCAL_WPE "Build the local WPE web application runner" ON)
set(OOS_LOCAL_WPE_SYSROOT
  "${OOS_REPO_ROOT}/build/wpe-sysroot/local-root/opt/oos"
  CACHE PATH "Pinned local WPE installation prefix")

set(OOS_LOCAL_KEYMAP_SOURCE
  "${CMAKE_CURRENT_SOURCE_DIR}/config/keymap.conf")
set(OOS_LOCAL_KEYMAP_BUILD
  "${CMAKE_BINARY_DIR}/config/local-keymap.conf")
configure_file("${OOS_LOCAL_KEYMAP_SOURCE}" "${OOS_LOCAL_KEYMAP_BUILD}" COPYONLY)

add_library(oos_local_platform STATIC
  src/device.cpp
  src/local_display.cpp
  src/local_key_input.cpp)
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

target_compile_definitions(oos PRIVATE
  OOS_DEFAULT_BOOT_SPLASH="${CMAKE_SOURCE_DIR}/assets/boot/nokia-2780-flip/boot-splash.png")
target_link_libraries(oos PRIVATE
  oos::platform
  oos_wasm_runtime
  PNG::PNG)

if(OOS_LOCAL_WPE)
  if(NOT EXISTS "${OOS_LOCAL_WPE_SYSROOT}/lib/libWPEWebKit-2.0.so")
    message(FATAL_ERROR
      "Local WPE sysroot is missing. Run 'make wpe-local' before configuring.")
  endif()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(OOS_LOCAL_WPE_HOST REQUIRED IMPORTED_TARGET
    glib-2.0 gobject-2.0 gio-2.0 libsoup-3.0 wayland-server)
  set(OOS_LOCAL_WPE_INCLUDES
    "${OOS_LOCAL_WPE_SYSROOT}/include/wpe-1.0"
    "${OOS_LOCAL_WPE_SYSROOT}/include/wpe-fdo-1.0"
    "${OOS_LOCAL_WPE_SYSROOT}/include/wpe-webkit-2.0"
    "${OOS_LOCAL_WPE_SYSROOT}/include/wpe-webkit-2.0/wpe-platform")
  set(OOS_LOCAL_WPE_LIBRARIES
    "${OOS_LOCAL_WPE_SYSROOT}/lib/libWPEWebKit-2.0.so"
    "${OOS_LOCAL_WPE_SYSROOT}/lib/libWPEBackend-fdo-1.0.so"
    "${OOS_LOCAL_WPE_SYSROOT}/lib/libwpe-1.0.so")

  add_library(oos_wpe_surface_host STATIC src/wpe_surface_host_fdo.cpp)
  target_include_directories(oos_wpe_surface_host PUBLIC
    "${CMAKE_SOURCE_DIR}/src" ${OOS_LOCAL_WPE_INCLUDES})
  target_link_libraries(oos_wpe_surface_host PUBLIC
    ${OOS_LOCAL_WPE_LIBRARIES} PkgConfig::OOS_LOCAL_WPE_HOST)
  add_library(oos::web ALIAS oos_wpe_surface_host)

  add_executable(oos-web-local src/web_runner.cpp)
  target_sources(oos-web-local PRIVATE
    "${CMAKE_SOURCE_DIR}/src/oos/web/kaios_api_bridge.cpp"
    "${CMAKE_SOURCE_DIR}/src/oos/web/local_app_server.cpp"
    "${CMAKE_SOURCE_DIR}/src/oos/web/wpe_app_profile.cpp")
  target_include_directories(oos-web-local PRIVATE ${OOS_LOCAL_WPE_INCLUDES})
  target_link_libraries(oos-web-local PRIVATE
    oos::platform oos::compositor oos::web
    oos_app_model oos_web_device_api
    ${OOS_LOCAL_WPE_LIBRARIES} PkgConfig::OOS_LOCAL_WPE_HOST)
  target_link_options(oos-web-local PRIVATE
    "-Wl,-rpath-link,${OOS_LOCAL_WPE_SYSROOT}/lib")
  set_target_properties(oos-web-local PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    BUILD_RPATH "${OOS_LOCAL_WPE_SYSROOT}/lib"
    INSTALL_RPATH "/opt/oos/lib")
endif()

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
endif()
