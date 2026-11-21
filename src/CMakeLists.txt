
# -------------------------------------------------------------
# Core

add_library(volt_core
  core/AppConfig.cpp
  core/Logging.cpp
  core/Text.cpp
)

target_include_directories(volt_core PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(volt_core PUBLIC spdlog::spdlog)

if(ICU_FOUND)
  if(TARGET ICU::i18n)
    target_link_libraries(volt_core PUBLIC ICU::i18n)
    if(TARGET ICU::uc)
      target_link_libraries(volt_core PUBLIC ICU::uc)
    endif()
    if(TARGET ICU::data)
      target_link_libraries(volt_core PUBLIC ICU::data)
    endif()
  elseif(DEFINED ICU_LIBRARIES)
    target_link_libraries(volt_core PUBLIC ${ICU_LIBRARIES})
    if(DEFINED ICU_INCLUDE_DIRS)
      target_include_directories(volt_core PUBLIC ${ICU_INCLUDE_DIRS})
    endif()
  endif()
  target_compile_definitions(volt_core PUBLIC VOLT_HAS_ICU=1)
else()
  target_compile_definitions(volt_core PUBLIC VOLT_HAS_ICU=0)
endif()

target_compile_definitions(volt_core PUBLIC
  $<$<CONFIG:Debug>:VOLT_ENABLE_LOGGING=1>
  $<$<CONFIG:Debug>:VOLT_ENABLE_DEBUG_LOGGING=1>
  $<$<NOT:$<CONFIG:Debug>>:VOLT_ENABLE_LOGGING=0>
  $<$<NOT:$<CONFIG:Debug>>:VOLT_ENABLE_DEBUG_LOGGING=0>
)

volt_set_warnings(volt_core)

# -------------------------------------------------------------
# Codec

add_library(volt_codec
  codec/compression/CanonicalHuffman.cpp
  codec/compression/DeflateCodec.cpp
  codec/compression/DeflateHuffman.cpp
  codec/compression/HuffmanCodec.cpp
  codec/compression/ZlibEnvelope.cpp
  codec/image/BmpCodec.cpp
  codec/image/ImageDecoder.cpp
  codec/image/ImageEncoder.cpp
  codec/image/JpegCodec.cpp
  codec/image/PngCodec.cpp
  codec/text/Json.cpp
)

target_include_directories(volt_codec PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(volt_codec PUBLIC volt_core)
volt_set_warnings(volt_codec)


# -------------------------------------------------------------
# Math

add_library(volt_math
  math/Math.cpp
)

target_include_directories(volt_math PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(volt_math PUBLIC volt_core)
volt_set_warnings(volt_math)

# -------------------------------------------------------------
# Physics

add_library(volt_physics
  physics/Physics.cpp
  physics/Spice.cpp
)

target_include_directories(volt_physics PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(volt_physics PUBLIC volt_core volt_math)
volt_set_warnings(volt_physics)

# -------------------------------------------------------------
# Event system

add_library(volt_event
  event/EventDispatcher.cpp
)

target_include_directories(volt_event PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(volt_event PUBLIC volt_core)
volt_set_warnings(volt_event)

# -------------------------------------------------------------
# Platform

add_library(volt_platform
  platform/platform.cpp
)

target_include_directories(volt_platform PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(volt_platform PUBLIC volt_event volt_core)
volt_set_warnings(volt_platform)

# -------------------------------------------------------------
# Renderer

add_library(volt_render
  render/renderer.cpp
)

target_include_directories(volt_render PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(volt_render PUBLIC Vulkan::Vulkan volt_event volt_core volt_math)

volt_set_warnings(volt_render)

# -------------------------------------------------------------
# Text

add_library(volt_text
  text/Utf.cpp
)

target_include_directories(volt_text PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(volt_text PUBLIC volt_core)


volt_set_warnings(volt_text)

# -------------------------------------------------------------
# UI

add_library(volt_ui
  ui/ui.cpp
)

target_include_directories(volt_ui PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(volt_ui PUBLIC volt_platform volt_event volt_core)
volt_set_warnings(volt_ui)

add_library(volt_io
  io/assets/Manifest.cpp
  io/import/ImportPipeline.cpp
  io/import/ImporterRegistry.cpp
  io/import/StepImporter.cpp
  io/import/ThreeMfImporter.cpp
)

target_include_directories(volt_io PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(volt_io PUBLIC volt_event volt_core volt_math volt_codec volt_text)

if(WIN32)
  target_link_libraries(volt_io PRIVATE windowscodecs ole32)
endif()

volt_set_warnings(volt_io)

target_link_libraries(volt_render PUBLIC volt_io)
target_link_libraries(volt_ui PUBLIC volt_io)

add_executable(volt_app
  app/main.cpp
)

target_link_libraries(volt_app PRIVATE volt_core volt_math volt_physics volt_event volt_platform volt_render volt_ui volt_io)
volt_set_warnings(volt_app)

find_program(VOLT_GLSLC_EXECUTABLE
  NAMES glslc
  HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/Bin32")

if(VOLT_GLSLC_EXECUTABLE)
  add_custom_command(TARGET volt_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:volt_app>/assets/shaders
    COMMAND ${VOLT_GLSLC_EXECUTABLE} ${PROJECT_SOURCE_DIR}/assets/shaders/scaffold.vert -o $<TARGET_FILE_DIR:volt_app>/assets/shaders/scaffold.vert.spv
    COMMAND ${VOLT_GLSLC_EXECUTABLE} ${PROJECT_SOURCE_DIR}/assets/shaders/scaffold.frag -o $<TARGET_FILE_DIR:volt_app>/assets/shaders/scaffold.frag.spv
    COMMENT "Compiling Vulkan shaders for runtime")
else()
  message(WARNING "glslc not found; Vulkan shader binaries will not be generated for volt_app runtime")
endif()

add_custom_command(TARGET volt_app POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:volt_app>/assets/images
  COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:volt_app>/assets/ui
  COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:volt_app>/assets/fonts
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PROJECT_SOURCE_DIR}/assets/manifest.json $<TARGET_FILE_DIR:volt_app>/assets/manifest.json
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_SOURCE_DIR}/assets/images $<TARGET_FILE_DIR:volt_app>/assets/images
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_SOURCE_DIR}/assets/ui $<TARGET_FILE_DIR:volt_app>/assets/ui
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_SOURCE_DIR}/assets/fonts $<TARGET_FILE_DIR:volt_app>/assets/fonts
  COMMENT "Staging runtime assets")
