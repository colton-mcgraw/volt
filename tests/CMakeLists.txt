add_executable(volt_compression_tests
  codec/compression/CompressionTests.cpp
)

target_link_libraries(volt_compression_tests PRIVATE volt_codec)
volt_set_warnings(volt_compression_tests)

add_test(NAME volt_compression_tests COMMAND volt_compression_tests)

add_executable(volt_huffman_codec_tests
  codec/compression/HuffmanCodecTests.cpp
)

target_link_libraries(volt_huffman_codec_tests PRIVATE volt_codec)
volt_set_warnings(volt_huffman_codec_tests)

add_test(NAME volt_huffman_codec_tests COMMAND volt_huffman_codec_tests)

add_executable(volt_compression_bench
  codec/compression/CompressionBenchmark.cpp
)

target_link_libraries(volt_compression_bench PRIVATE volt_codec)
volt_set_warnings(volt_compression_bench)

add_executable(volt_huffman_codec_bench
  codec/compression/HuffmanCodecBenchmark.cpp
)

target_link_libraries(volt_huffman_codec_bench PRIVATE volt_codec)
volt_set_warnings(volt_huffman_codec_bench)

add_executable(volt_image_codec_tests
  codec/image/ImageCodecTests.cpp
)

target_link_libraries(volt_image_codec_tests PRIVATE volt_codec)
volt_set_warnings(volt_image_codec_tests)

add_test(NAME volt_image_codec_tests COMMAND volt_image_codec_tests)

add_executable(volt_jpeg_bench
  codec/image/JpegPerformanceTests.cpp
)

target_link_libraries(volt_jpeg_bench PRIVATE volt_codec)
volt_set_warnings(volt_jpeg_bench)

add_executable(volt_utf_tests
  text/UtfTests.cpp
)

target_link_libraries(volt_utf_tests PRIVATE volt_text)
volt_set_warnings(volt_utf_tests)

add_test(NAME volt_utf_tests COMMAND volt_utf_tests)

add_executable(volt_json_tests
  codec/text/JsonTests.cpp
)

target_link_libraries(volt_json_tests PRIVATE volt_codec)
volt_set_warnings(volt_json_tests)

add_test(NAME volt_json_tests COMMAND volt_json_tests)

add_executable(volt_manifest_tests
  io/assets/ManifestTests.cpp
  ../src/io/assets/Manifest.cpp
)

target_link_libraries(volt_manifest_tests PRIVATE volt_codec)
volt_set_warnings(volt_manifest_tests)

add_test(NAME volt_manifest_tests COMMAND volt_manifest_tests)

add_executable(volt_json_bench
  codec/text/JsonBenchmark.cpp
)

target_link_libraries(volt_json_bench PRIVATE volt_codec)
volt_set_warnings(volt_json_bench)

function(volt_add_bench_runner executable_target)
  add_custom_target(run_${executable_target}
    COMMAND $<TARGET_FILE:${executable_target}>
    DEPENDS ${executable_target}
    USES_TERMINAL
    COMMENT "Running ${executable_target}")
endfunction()

volt_add_bench_runner(volt_compression_bench)
volt_add_bench_runner(volt_huffman_codec_bench)
volt_add_bench_runner(volt_jpeg_bench)
volt_add_bench_runner(volt_json_bench)

add_custom_target(run_volt_codec_benches
  DEPENDS
    run_volt_compression_bench
    run_volt_huffman_codec_bench
    run_volt_jpeg_bench
    run_volt_json_bench)
