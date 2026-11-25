# volt_json: JSON text in, plain values out, and back. Strict on reading,
# deterministic on writing, so that the files Volt keeps (libraries,
# projects, settings) read well, diff well and round-trip unchanged. With
# the safe file replacement those files are saved with.
add_library(volt_json STATIC
  json.cpp
)

target_link_libraries(volt_json PUBLIC volt_core)
volt_set_warnings(volt_json)
