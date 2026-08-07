function(oos_add_quickjs target source_dir)
  if(NOT EXISTS "${source_dir}/quickjs.h" OR
     NOT EXISTS "${source_dir}/quickjs.c" OR
     NOT EXISTS "${source_dir}/VERSION")
    message(FATAL_ERROR
      "QuickJS is missing. Run scripts/fetch-quickjs.sh before configuring OOS.")
  endif()

  file(READ "${source_dir}/VERSION" quickjs_version)
  string(STRIP "${quickjs_version}" quickjs_version)
  add_library(${target} STATIC
    "${source_dir}/quickjs.c"
    "${source_dir}/dtoa.c"
    "${source_dir}/libregexp.c"
    "${source_dir}/libunicode.c"
    "${source_dir}/cutils.c")
  target_include_directories(${target} SYSTEM PUBLIC "${source_dir}")
  target_compile_definitions(${target} PRIVATE
    _GNU_SOURCE CONFIG_VERSION="${quickjs_version}")
  target_compile_options(${target} PRIVATE
    -fwrapv
    -Wno-array-bounds
    -Wno-format-truncation
    -Wno-infinite-recursion
    -Wno-sign-compare
    -Wno-unused-parameter)
  target_link_libraries(${target} PUBLIC m)
endfunction()
