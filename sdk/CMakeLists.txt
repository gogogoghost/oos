add_library(oos_sdk_ui STATIC
  cpp/src/fonts.cpp
  cpp/src/imgui_backend.cpp
  cpp/src/lvgl_backend.cpp
  cpp/src/oos_icon_font_12.c
  cpp/src/oos_icon_font_14.c
  cpp/src/oos_icon_font_20.c)
target_include_directories(oos_sdk_ui PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/cpp/include")
target_compile_options(oos_sdk_ui PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti>)
target_link_libraries(oos_sdk_ui PUBLIC
  lvgl oos_imgui_core oos_compositor oos_input)
add_library(oos::sdk::ui ALIAS oos_sdk_ui)
