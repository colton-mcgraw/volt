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
