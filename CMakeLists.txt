cmake_minimum_required(VERSION 3.25)

project(
  Volt
  VERSION 0.1.0
  DESCRIPTION "Modern cross-platform electronics design platform"
  LANGUAGES CXX
)

include(cmake/CompilerWarnings.cmake)

option(VOLT_ENABLE_ICU "Enable ICU-backed Unicode normalization/collation" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

find_package(Vulkan REQUIRED)
find_package(spdlog CONFIG QUIET)
if(VOLT_ENABLE_ICU)
  find_package(ICU COMPONENTS uc i18n QUIET)
  if(ICU_FOUND)
    message(STATUS "ICU Unicode backend enabled")
  else()
    message(STATUS "ICU requested but not found; using fallback Unicode behavior")
  endif()
else()
  message(STATUS "ICU Unicode backend disabled; using fallback Unicode behavior")
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

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
