function(volt_set_warnings target_name)
  if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    # clang-cl: cl-style command line.
    target_compile_options(${target_name} PRIVATE /W4 /permissive- /EHsc)
  else()
    target_compile_options(
      ${target_name}
      PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wconversion
      -Wshadow
      -Wnon-virtual-dtor
      -Wold-style-cast
      -Wnull-dereference
      -Wdouble-promotion
    )
  endif()
endfunction()
