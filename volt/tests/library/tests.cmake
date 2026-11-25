# Tests of the library module: parts and .voltlib files, the catalog and its
# search, the project file and project library, the schema and its checks,
# editing and part packages, and the seed libraries in
# assets/libraries (their directory, and the models directory their parts
# point into, are compiled in, so the tests run from anywhere).

set(volt_library_test_assets "${PROJECT_SOURCE_DIR}/assets/libraries")

foreach(suite IN ITEMS library catalog project schema package)
  volt_add_test(volt_library_${suite}_tests library/${suite}_tests.cpp)
  target_link_libraries(volt_library_${suite}_tests PRIVATE volt_library)
endforeach()

# The seed libraries load clean and are what their generator writes today;
# the generator's source is compiled in to compare.
volt_add_test(volt_library_seed_tests
  library/seed_tests.cpp
  ${PROJECT_SOURCE_DIR}/volt/src/library/tools/seed.cpp)
target_link_libraries(volt_library_seed_tests PRIVATE volt_library)
target_include_directories(volt_library_seed_tests PRIVATE ${PROJECT_SOURCE_DIR}/volt/src/library/tools)
target_compile_definitions(volt_library_seed_tests PRIVATE
  VOLT_LIBRARY_ASSETS="${volt_library_test_assets}"
  VOLT_MODEL_ASSETS="${PROJECT_SOURCE_DIR}/assets/models")

# Search and indexing times over about 50 000 parts (the seed libraries,
# copied). Build it in Release for numbers that mean something.
volt_add_bench(volt_library_bench library/library_bench.cpp)
target_link_libraries(volt_library_bench PRIVATE volt_library)
target_compile_definitions(volt_library_bench PRIVATE VOLT_LIBRARY_ASSETS="${volt_library_test_assets}")
