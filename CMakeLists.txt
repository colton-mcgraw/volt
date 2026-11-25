cmake_minimum_required(VERSION 3.25)

project(
  Volt
  VERSION 0.3.0
  DESCRIPTION "Modern cross-platform Volt electronics design platform"
  LANGUAGES CXX
)

# Clang is the only supported toolchain on every platform (clang++ or clang-cl,
# including Apple Clang). Use the presets in CMakePresets.json to select it.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang$")
  message(FATAL_ERROR
    "Volt requires Clang, but the selected compiler is "
    "${CMAKE_CXX_COMPILER_ID} (${CMAKE_CXX_COMPILER}).\n"
    "Configure with a preset, e.g. `cmake --preset windows-clang`, "
    "or pass -DCMAKE_CXX_COMPILER=clang++.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Put every executable in one place per configuration so runtime assets staged
# next to volt_app are found regardless of generator.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")

include(cmake/CompilerWarnings.cmake)
include(CTest)

# glslc (part of the Vulkan SDK) compiles the shaders into the app, see volt/src.
find_package(Vulkan REQUIRED COMPONENTS glslc)

add_subdirectory(volt/src)

if(BUILD_TESTING)
  add_subdirectory(volt/tests)
endif()
