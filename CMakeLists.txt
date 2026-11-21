cmake_minimum_required(VERSION 3.25)

project(
  Volt
  VERSION 0.1.0
  DESCRIPTION "Modern cross-platform Volt electronics design platform"
  LANGUAGES CXX
)

include(cmake/CompilerWarnings.cmake)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

find_package(Vulkan REQUIRED)

add_subdirectory(src)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
