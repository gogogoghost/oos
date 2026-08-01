set(OOS_LAUNCHER_LOGO_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/launcher/assets/oos-logo-32-bgra.bin")
if(NOT EXISTS "${OOS_LAUNCHER_LOGO_SOURCE}")
  message(FATAL_ERROR
    "Missing Launcher logo. Run scripts/generate-brand-assets.sh first.")
endif()
file(SIZE "${OOS_LAUNCHER_LOGO_SOURCE}" OOS_LAUNCHER_LOGO_SIZE)
if(NOT OOS_LAUNCHER_LOGO_SIZE EQUAL 4096)
  message(FATAL_ERROR "Launcher logo must be a 32x32 BGRA image.")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${OOS_LAUNCHER_LOGO_SOURCE}")
file(READ "${OOS_LAUNCHER_LOGO_SOURCE}" OOS_LAUNCHER_LOGO_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1,"
  OOS_LAUNCHER_LOGO_BYTES "${OOS_LAUNCHER_LOGO_HEX}")
set(OOS_LAUNCHER_GENERATED_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/launcher/generated")
file(MAKE_DIRECTORY "${OOS_LAUNCHER_GENERATED_DIR}")
configure_file(
  launcher/src/embedded_logo.cpp.in
  "${OOS_LAUNCHER_GENERATED_DIR}/embedded_logo.cpp"
  @ONLY)
unset(OOS_LAUNCHER_LOGO_HEX)
unset(OOS_LAUNCHER_LOGO_BYTES)
unset(OOS_LAUNCHER_LOGO_SIZE)

add_library(oos_launcher_app STATIC
  launcher/src/launcher.cpp
  "${OOS_LAUNCHER_GENERATED_DIR}/embedded_logo.cpp")
target_include_directories(oos_launcher_app PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/launcher/include")
target_compile_options(oos_launcher_app PRIVATE -fno-exceptions -fno-rtti)
target_link_libraries(oos_launcher_app PUBLIC
  oos_sdk_ui oos_app_model oos_window)

add_library(oos_systemui_app STATIC
  systemui/src/system_ui.cpp)
target_include_directories(oos_systemui_app PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/systemui/include")
target_compile_options(oos_systemui_app PRIVATE -fno-exceptions -fno-rtti)
target_link_libraries(oos_systemui_app PUBLIC
  oos_sdk_ui oos_compositor oos_window)
