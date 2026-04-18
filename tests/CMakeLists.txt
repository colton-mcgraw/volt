add_executable(volt_json_tests
  io/text/JsonTests.cpp
)

target_link_libraries(volt_json_tests PRIVATE volt_io)
volt_set_warnings(volt_json_tests)

add_test(NAME volt_json_tests COMMAND volt_json_tests)

add_executable(volt_compression_tests
  io/compression/CompressionTests.cpp
)

target_link_libraries(volt_compression_tests PRIVATE volt_io)
volt_set_warnings(volt_compression_tests)

add_test(NAME volt_compression_tests COMMAND volt_compression_tests)

add_executable(volt_huffman_codec_tests
  io/compression/HuffmanCodecTests.cpp
)

target_link_libraries(volt_huffman_codec_tests PRIVATE volt_io)
volt_set_warnings(volt_huffman_codec_tests)

add_test(NAME volt_huffman_codec_tests COMMAND volt_huffman_codec_tests)

add_executable(volt_huffman_codec_bench
  io/compression/HuffmanCodecBenchmark.cpp
)

target_link_libraries(volt_huffman_codec_bench PRIVATE volt_io)
volt_set_warnings(volt_huffman_codec_bench)

add_executable(volt_image_codec_tests
  io/image/ImageCodecTests.cpp
)

target_link_libraries(volt_image_codec_tests PRIVATE volt_io)
volt_set_warnings(volt_image_codec_tests)

add_test(NAME volt_image_codec_tests COMMAND volt_image_codec_tests)

add_executable(volt_jpeg_perf_tests
  io/image/JpegPerformanceTests.cpp
)

target_link_libraries(volt_jpeg_perf_tests PRIVATE volt_io)
volt_set_warnings(volt_jpeg_perf_tests)

add_test(NAME volt_jpeg_perf_tests COMMAND volt_jpeg_perf_tests)

add_executable(volt_font_codec_tests
  io/assets/FontCodecTests.cpp
)

target_link_libraries(volt_font_codec_tests PRIVATE volt_io)
target_include_directories(volt_font_codec_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
volt_set_warnings(volt_font_codec_tests)

add_test(NAME volt_font_codec_tests COMMAND volt_font_codec_tests)

find_program(VOLT_TEST_GLSLC_EXECUTABLE
  NAMES glslc
  HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/Bin32")

if(VOLT_TEST_GLSLC_EXECUTABLE)
  add_executable(volt_font_gpu_atlas_tests
    io/assets/FontGpuAtlasRenderTests.cpp
  )

  target_link_libraries(volt_font_gpu_atlas_tests PRIVATE Vulkan::Vulkan volt_io)
  target_include_directories(volt_font_gpu_atlas_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
  volt_set_warnings(volt_font_gpu_atlas_tests)

  add_custom_command(TARGET volt_font_gpu_atlas_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:volt_font_gpu_atlas_tests>/assets/shaders
    COMMAND ${VOLT_TEST_GLSLC_EXECUTABLE} ${PROJECT_SOURCE_DIR}/assets/shaders/msdfgen.comp -o $<TARGET_FILE_DIR:volt_font_gpu_atlas_tests>/assets/shaders/msdfgen.comp.spv
    COMMENT "Compiling font compute shader for tests")

  add_test(NAME volt_font_gpu_atlas_tests COMMAND volt_font_gpu_atlas_tests)
else()
  message(WARNING "glslc not found; volt_font_gpu_atlas_tests will not be added")
endif()

add_executable(volt_font_atlas_bench
  io/assets/SfntFontAtlasBenchmark.cpp
)

target_link_libraries(volt_font_atlas_bench PRIVATE volt_io)
target_include_directories(volt_font_atlas_bench PRIVATE ${PROJECT_SOURCE_DIR}/src)
volt_set_warnings(volt_font_atlas_bench)

add_executable(volt_utf_tests
  io/text/UtfTests.cpp
)

target_link_libraries(volt_utf_tests PRIVATE volt_io)
volt_set_warnings(volt_utf_tests)

add_test(NAME volt_utf_tests COMMAND volt_utf_tests)

add_executable(volt_text_tests
  core/TextTests.cpp
)

target_link_libraries(volt_text_tests PRIVATE volt_core)
volt_set_warnings(volt_text_tests)

add_test(NAME volt_text_tests COMMAND volt_text_tests)

add_executable(volt_ui_tests
  ui/UILayerTests.cpp
)

target_link_libraries(volt_ui_tests PRIVATE volt_ui)
volt_set_warnings(volt_ui_tests)

add_test(NAME volt_ui_tests COMMAND volt_ui_tests)

add_executable(volt_app_config_tests
  core/AppConfigTests.cpp
)

target_link_libraries(volt_app_config_tests PRIVATE volt_core volt_io)
volt_set_warnings(volt_app_config_tests)

add_test(NAME volt_app_config_tests COMMAND volt_app_config_tests)

if(WIN32)
  add_executable(volt_win32_window_backend_tests
    platform/Win32WindowBackendTests.cpp
  )

  target_include_directories(volt_win32_window_backend_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
  volt_set_warnings(volt_win32_window_backend_tests)

  add_test(NAME volt_win32_window_backend_tests COMMAND volt_win32_window_backend_tests)
endif()
