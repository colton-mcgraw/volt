# Tests of the model module. The sample files they read live in
# assets/models/tests; the directory is compiled in, so the tests run from
# anywhere.

set(volt_model_test_assets "${PROJECT_SOURCE_DIR}/assets/models/tests")

function(volt_add_model_test name)
  volt_add_test(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE volt_model)
  target_compile_definitions(${name} PRIVATE VOLT_MODEL_TEST_ASSETS="${volt_model_test_assets}")
endfunction()

volt_add_model_test(volt_model_part21_tests model/part21_tests.cpp)
volt_add_model_test(volt_model_step_tests model/step_tests.cpp)
volt_add_model_test(volt_model_formats_tests model/formats_tests.cpp)
volt_add_model_test(volt_model_robustness_tests model/robustness_tests.cpp)

# The private geometry layer on its own: it is not part of the public
# headers, so this one test looks into the library's sources.
volt_add_model_test(volt_model_geometry_tests model/geometry_tests.cpp)
target_include_directories(volt_model_geometry_tests PRIVATE ${PROJECT_SOURCE_DIR}/volt/src/model)

# Timings: Part 21 throughput on a large generated file, tessellation of the
# test shapes, and any STEP files named on the command line.
volt_add_bench(volt_model_bench model/model_bench.cpp)
target_link_libraries(volt_model_bench PRIVATE volt_model)
target_compile_definitions(volt_model_bench PRIVATE VOLT_MODEL_TEST_ASSETS="${volt_model_test_assets}")

# Not a test: renders models to PNG files with a software rasterizer, to look
# at what the loaders make of them (`volt_model_view all <directory>`).
add_executable(volt_model_view model/model_view.cpp)
target_include_directories(volt_model_view PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(volt_model_view PRIVATE volt_model)
target_compile_definitions(volt_model_view PRIVATE VOLT_MODEL_TEST_ASSETS="${volt_model_test_assets}")
volt_set_warnings(volt_model_view)
