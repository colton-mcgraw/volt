# Test and benchmark sources live next to this file and can include "shared.hpp".
#
#   volt_add_test(volt_event_bus_tests events/BusTests.cpp)
#   volt_add_bench(volt_event_bus_bench events/BusBenchmark.cpp)
#
# Tests are registered with CTest; benchmarks get a `run_<name>` target and are
# all run by `run_volt_benches`.

function(volt_add_test name)
  add_executable(${name} ${ARGN})
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_FUNCTION_LIST_DIR})
  target_link_libraries(${name} PRIVATE volt_core)
  volt_set_warnings(${name})
  add_test(NAME ${name} COMMAND ${name})
endfunction()

add_custom_target(run_volt_benches)

function(volt_add_bench name)
  add_executable(${name} ${ARGN})
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_FUNCTION_LIST_DIR})
  target_link_libraries(${name} PRIVATE volt_core)
  volt_set_warnings(${name})

  add_custom_target(run_${name}
    COMMAND $<TARGET_FILE:${name}>
    DEPENDS ${name}
    USES_TERMINAL
    COMMENT "Running ${name}")
  add_dependencies(run_volt_benches run_${name})
endfunction()

volt_add_test(volt_math_vector_tests math/vector_tests.cpp)
volt_add_test(volt_math_matrix_tests math/matrix_tests.cpp)
volt_add_test(volt_math_transform_tests math/transform_tests.cpp)
volt_add_test(volt_math_fraction_tests math/fraction_tests.cpp)
volt_add_test(volt_math_electrical_tests math/electrical_tests.cpp)

# <gpu/color.hpp> is plain math, so it is tested without a device.
volt_add_test(volt_gpu_color_tests gpu/color_tests.cpp)

# Modules that bring more than a file or two list their own tests.
include(platform/tests.cmake)
