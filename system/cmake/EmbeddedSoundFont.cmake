function(oos_embed_gm_soundfont target soundfont_file)
  if(NOT EXISTS "${soundfont_file}")
    message(FATAL_ERROR "GM SoundFont is missing: ${soundfont_file}")
  endif()
  file(TO_CMAKE_PATH "${soundfont_file}" OOS_GM_SOUNDFONT_FILE)
  set(generated "${CMAKE_CURRENT_BINARY_DIR}/${target}-gm-soundfont.cpp")
  configure_file(
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/oos/media/embedded_soundfont.cpp.in"
    "${generated}" @ONLY)
  target_sources(${target} PRIVATE "${generated}")
endfunction()
