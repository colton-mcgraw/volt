# volt_library: the parts library. Parts and the .voltlib files that hold
# them, the catalog of every library the application knows (lookup, the
# category tree, search), and the .voltproj project file with the project's
# own copy of the parts it uses; the schema that says what a part's
# parameters mean and checks them, editing a library, and the .voltpart
# packages parts are imported from. Plain data on files: no GPU, no window, no
# interface, and usable from a worker thread.
add_library(volt_library STATIC
  part.cpp
  library.cpp
  catalog.cpp
  project.cpp
  schema.cpp
  edit.cpp
  package.cpp
)

target_link_libraries(volt_library PUBLIC volt_core volt_json)
volt_set_warnings(volt_library)

# volt_parts: checking, importing and exporting parts from the command
# line, which is also how a third-party tool hands parts to a library (see
# tools/parts_main.cpp and <library/package.hpp>).
add_executable(volt_parts tools/parts_main.cpp)
target_link_libraries(volt_parts PRIVATE volt_library)
volt_set_warnings(volt_parts)

# The generator of the libraries shipped in assets/libraries. They are
# committed files; after changing the generator, rebuild them with
#
#   cmake --build <build> --target volt_generate_libraries
#
# and commit the result. A test checks that the two agree.
add_executable(volt_library_seed EXCLUDE_FROM_ALL
  tools/seed.cpp
  tools/seed_main.cpp
)
target_link_libraries(volt_library_seed PRIVATE volt_library)
volt_set_warnings(volt_library_seed)

add_custom_target(volt_generate_libraries
  COMMAND volt_library_seed ${PROJECT_SOURCE_DIR}/assets/libraries
  DEPENDS volt_library_seed
  COMMENT "Writing the seed libraries to assets/libraries"
  VERBATIM)
