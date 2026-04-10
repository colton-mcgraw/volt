cmake_minimum_required(VERSION 3.25)

project(
  Volt
  VERSION 0.1.0
  DESCRIPTION "Modern cross-platform Volt electronics design platform"
  LANGUAGES CXX
)

include(cmake/CompilerWarnings.cmake)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Vulkan REQUIRED)
find_package(glfw3 CONFIG QUIET)
find_package(spdlog CONFIG QUIET)

if(NOT glfw3_FOUND)
  include(FetchContent)
  FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
  )
  FetchContent_MakeAvailable(glfw)
endif()

if(NOT spdlog_FOUND)
  include(FetchContent)
  FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.14.1
  )
  FetchContent_MakeAvailable(spdlog)
endif()

add_subdirectory(src)
