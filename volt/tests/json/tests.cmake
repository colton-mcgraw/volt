# Tests of the JSON module: reading (and refusing), error positions, limits,
# writing, round trips, and the safe file replacement.
volt_add_test(volt_json_tests json/json_tests.cpp)
target_link_libraries(volt_json_tests PRIVATE volt_json)
