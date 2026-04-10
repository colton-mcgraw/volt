add_library(volt_core
  core/AppConfig.cpp
  core/Logging.cpp
)

target_include_directories(volt_core PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(volt_core PUBLIC spdlog::spdlog)

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
)

target_include_directories(volt_platform PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(volt_platform PUBLIC glfw volt_event volt_core)
volt_set_warnings(volt_platform)

add_library(volt_render
  render/VulkanRenderer.cpp
)

target_include_directories(volt_render PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(volt_render PUBLIC Vulkan::Vulkan glfw volt_event volt_core volt_math volt_io)

volt_set_warnings(volt_render)

add_library(volt_ui
  ui/UIMesh.cpp
  ui/UIStyle.cpp
  ui/UIText.cpp
  ui/UILayer.cpp
)

target_include_directories(volt_ui PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_include_directories(volt_ui PRIVATE ${PROJECT_SOURCE_DIR}/third_party)
target_link_libraries(volt_ui PUBLIC volt_platform volt_event volt_core volt_io)
volt_set_warnings(volt_ui)

add_library(volt_io
  io/Manifest.cpp
  io/ImageCodec.cpp
  io/ImageAssets.cpp
  io/ImportPipeline.cpp
  io/ImporterRegistry.cpp
  io/StepImporter.cpp
  io/ThreeMfImporter.cpp
)

target_include_directories(volt_io PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_include_directories(volt_io PRIVATE ${PROJECT_SOURCE_DIR}/third_party)
target_link_libraries(volt_io PUBLIC volt_event volt_core volt_math)

volt_set_warnings(volt_io)

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
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PROJECT_SOURCE_DIR}/assets/images/manifest.txt $<TARGET_FILE_DIR:volt_app>/assets/images/manifest.txt
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PROJECT_SOURCE_DIR}/assets/volt.png $<TARGET_FILE_DIR:volt_app>/assets/volt.png
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PROJECT_SOURCE_DIR}/assets/ui/default.style $<TARGET_FILE_DIR:volt_app>/assets/ui/default.style
  COMMENT "Staging runtime UI and image assets")
