# Tests of the image module. Each gets the assets directory (the test
# pictures are in assets/images) and a directory in the build tree where it
# leaves what it rendered, for a person to look at.

function(volt_add_image_test name source)
  volt_add_test(${name} ${source})
  target_link_libraries(${name} PRIVATE volt_image)
  target_compile_definitions(${name} PRIVATE
    VOLT_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets"
    VOLT_TEST_OUTPUT_DIR="${CMAKE_BINARY_DIR}/image-test-output")
endfunction()

volt_add_image_test(volt_image_tests image/image_tests.cpp)
volt_add_image_test(volt_image_compress_tests image/compress_tests.cpp)
volt_add_image_test(volt_image_png_tests image/png_tests.cpp)
volt_add_image_test(volt_image_jpeg_tests image/jpeg_tests.cpp)
volt_add_image_test(volt_image_svg_tests image/svg_tests.cpp)
