function(oos_add_ffmpeg target prefix)
  foreach(library avformat avcodec swresample avutil)
    set(imported_target "${target}_${library}")
    add_library(${imported_target} SHARED IMPORTED)
    set_target_properties(${imported_target} PROPERTIES
      IMPORTED_LOCATION "${prefix}/lib/lib${library}.so"
      INTERFACE_INCLUDE_DIRECTORIES "${prefix}/include")
  endforeach()
  add_library(${target} INTERFACE)
  target_link_libraries(${target} INTERFACE
    ${target}_avformat
    ${target}_avcodec
    ${target}_swresample
    ${target}_avutil
    m)
  if(UNIX AND NOT ANDROID)
    target_link_libraries(${target} INTERFACE pthread)
  endif()
endfunction()
