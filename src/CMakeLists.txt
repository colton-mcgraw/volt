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

add_library(volt_math
  math/Math.cpp
)

target_include_directories(volt_math PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(volt_math PUBLIC volt_core)
volt_set_warnings(volt_math)

add_library(volt_physics
  physics/Physics.cpp
  physics/Spice.cpp
)

target_include_directories(volt_physics PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(volt_physics PUBLIC volt_core volt_math)
volt_set_warnings(volt_physics)

add_library(volt_event
  event/EventDispatcher.cpp
)

target_include_directories(volt_event PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(volt_event PUBLIC volt_core)
volt_set_warnings(volt_event)

add_library(volt_platform
  platform/Window.cpp
  platform/WindowBackendFactory.cpp
  platform/details/Win32WindowBackend.cpp
  platform/details/LinuxWindowBackend.cpp
  platform/details/MacOSWindowBackend.cpp
)

target_include_directories(volt_platform PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(volt_platform PUBLIC volt_event volt_core)
if(WIN32)
  target_link_libraries(volt_platform PUBLIC user32 dwmapi shcore)
endif()
volt_set_warnings(volt_platform)

add_library(volt_render
  render/RendererFactory.cpp
  render/details/VulkanRenderer.cpp
  render/details/VectorTextWorker.cpp
)

target_include_directories(volt_render PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(volt_render PUBLIC Vulkan::Vulkan volt_event volt_core volt_math volt_io)
if(WIN32)
  target_compile_definitions(volt_render PRIVATE VK_USE_PLATFORM_WIN32_KHR NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

volt_set_warnings(volt_render)

add_library(volt_ui
  ui/UIMesh.cpp
  ui/UIStyle.cpp
  ui/UIText.cpp
  ui/UILayer.cpp
)

target_include_directories(volt_ui PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(volt_ui PUBLIC volt_platform volt_event volt_core volt_io)
volt_set_warnings(volt_ui)

add_library(volt_io
  io/assets/AssetManager.cpp
  io/assets/ImageAssetService.cpp
  io/assets/ModelAssetService.cpp
  io/assets/FontAssetService.cpp
  io/assets/SfntFontAtlas.cpp
  io/compression/CanonicalHuffman.cpp
  io/compression/DeflateHuffman.cpp
  io/compression/ZlibEnvelope.cpp
  io/compression/DeflateCodec.cpp
  io/assets/Font.cpp
  io/compression/HuffmanCodec.cpp
  io/image/BmpCodec.cpp
  io/image/PngCodec.cpp
  io/image/ImageDecoder.cpp
  io/image/ImageEncoder.cpp
  io/image/JpegCodec.cpp
  io/assets/Manifest.cpp
  io/text/Json.cpp
  io/import/ImportPipeline.cpp
  io/import/ImporterRegistry.cpp
  io/import/StepImporter.cpp
  io/import/ThreeMfImporter.cpp
  io/text/Utf.cpp
)

target_include_directories(volt_io PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(volt_io PUBLIC volt_event volt_core volt_math)

if(WIN32)
  target_link_libraries(volt_io PRIVATE windowscodecs ole32)
endif()

volt_set_warnings(volt_io)

add_executable(volt_app
  app/AppUi.cpp
  app/main.cpp
)

target_link_libraries(volt_app PRIVATE volt_core volt_math volt_physics volt_event volt_platform volt_render volt_ui volt_io)
volt_set_warnings(volt_app)

set(VOLT_DEFAULT_EMBEDDED_FONT "${PROJECT_SOURCE_DIR}/assets/fonts/DefaultFont.ttf")
if(NOT EXISTS "${VOLT_DEFAULT_EMBEDDED_FONT}")
  message(FATAL_ERROR "Required embedded default font is missing: ${VOLT_DEFAULT_EMBEDDED_FONT}")
endif()

find_program(VOLT_GLSLC_EXECUTABLE
  NAMES glslc
  HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/Bin32")

if(VOLT_GLSLC_EXECUTABLE)
  add_custom_command(TARGET volt_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:volt_app>/assets/shaders
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:volt_app>/assets/shaders/text
    COMMAND ${VOLT_GLSLC_EXECUTABLE} ${PROJECT_SOURCE_DIR}/assets/shaders/ui.vert -o $<TARGET_FILE_DIR:volt_app>/assets/shaders/ui.vert.spv
    COMMAND ${VOLT_GLSLC_EXECUTABLE} ${PROJECT_SOURCE_DIR}/assets/shaders/scaffold.vert -o $<TARGET_FILE_DIR:volt_app>/assets/shaders/scaffold.vert.spv
    COMMAND ${VOLT_GLSLC_EXECUTABLE} ${PROJECT_SOURCE_DIR}/assets/shaders/scaffold.frag -o $<TARGET_FILE_DIR:volt_app>/assets/shaders/scaffold.frag.spv
    COMMAND ${VOLT_GLSLC_EXECUTABLE} ${PROJECT_SOURCE_DIR}/assets/shaders/msdfgen.comp -o $<TARGET_FILE_DIR:volt_app>/assets/shaders/msdfgen.comp.spv
    COMMAND ${VOLT_GLSLC_EXECUTABLE} ${PROJECT_SOURCE_DIR}/assets/shaders/text.frag -o $<TARGET_FILE_DIR:volt_app>/assets/shaders/text.frag.spv
    COMMAND ${VOLT_GLSLC_EXECUTABLE} ${PROJECT_SOURCE_DIR}/assets/shaders/ui_text_sdf.frag -o $<TARGET_FILE_DIR:volt_app>/assets/shaders/ui_text_sdf.frag.spv
    COMMAND ${VOLT_GLSLC_EXECUTABLE} ${PROJECT_SOURCE_DIR}/assets/shaders/text/flatten_count.comp -o $<TARGET_FILE_DIR:volt_app>/assets/shaders/text/flatten_count.comp.spv
    COMMAND ${VOLT_GLSLC_EXECUTABLE} ${PROJECT_SOURCE_DIR}/assets/shaders/text/flatten_emit.comp -o $<TARGET_FILE_DIR:volt_app>/assets/shaders/text/flatten_emit.comp.spv
    COMMAND ${VOLT_GLSLC_EXECUTABLE} ${PROJECT_SOURCE_DIR}/assets/shaders/text/bin_count.comp -o $<TARGET_FILE_DIR:volt_app>/assets/shaders/text/bin_count.comp.spv
    COMMAND ${VOLT_GLSLC_EXECUTABLE} ${PROJECT_SOURCE_DIR}/assets/shaders/text/bin_emit.comp -o $<TARGET_FILE_DIR:volt_app>/assets/shaders/text/bin_emit.comp.spv
    COMMAND ${VOLT_GLSLC_EXECUTABLE} ${PROJECT_SOURCE_DIR}/assets/shaders/text/fine.comp -o $<TARGET_FILE_DIR:volt_app>/assets/shaders/text/fine.comp.spv
    COMMAND ${VOLT_GLSLC_EXECUTABLE} ${PROJECT_SOURCE_DIR}/assets/shaders/text/prefix_scan.comp -o $<TARGET_FILE_DIR:volt_app>/assets/shaders/text/prefix_scan.comp.spv
    COMMAND ${VOLT_GLSLC_EXECUTABLE} ${PROJECT_SOURCE_DIR}/assets/shaders/text/exclusive_scan.comp -o $<TARGET_FILE_DIR:volt_app>/assets/shaders/text/exclusive_scan.comp.spv
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
