function(oos_add_fluidlite source_dir)
  if(TARGET fluidlite)
    return()
  endif()

  cmake_policy(PUSH)
  cmake_policy(SET CMP0077 NEW)
  # WAMR configures a freestanding link environment globally. FluidLite's
  # header probes only need compilation, and must not inherit that link step.
  set(saved_try_compile_target_type "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
  set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
  unset(STDC_HEADERS CACHE)
  unset(HAVE_FCNTL_H CACHE)
  set(ENABLE_SF3 OFF CACHE BOOL "" FORCE)
  set(WITH_FLOAT ON CACHE BOOL "" FORCE)
  set(FLUIDLITE_BUILD_STATIC OFF CACHE BOOL "" FORCE)
  set(FLUIDLITE_BUILD_SHARED ON CACHE BOOL "" FORCE)
  add_subdirectory("${source_dir}"
                   "${CMAKE_CURRENT_BINARY_DIR}/third_party/fluidlite"
                   EXCLUDE_FROM_ALL)
  target_compile_options(fluidlite PRIVATE
    -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable)
  set(CMAKE_TRY_COMPILE_TARGET_TYPE "${saved_try_compile_target_type}")
  cmake_policy(POP)
endfunction()
