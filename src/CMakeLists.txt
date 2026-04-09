add_library(volt_core
  core/AppConfig.cpp
)

target_include_directories(volt_core PUBLIC ${PROJECT_SOURCE_DIR}/include)
volt_set_warnings(volt_core)

add_library(volt_platform
  platform/Window.cpp
)

target_include_directories(volt_platform PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(volt_platform PUBLIC glfw)
volt_set_warnings(volt_platform)

add_library(volt_render
  render/VulkanRenderer.cpp
)

target_include_directories(volt_render PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(volt_render PUBLIC Vulkan::Vulkan glfw)
volt_set_warnings(volt_render)

add_library(volt_io
  io/ImportPipeline.cpp
  io/ImporterRegistry.cpp
  io/StepImporter.cpp
  io/ThreeMfImporter.cpp
)

target_include_directories(volt_io PUBLIC ${PROJECT_SOURCE_DIR}/include)

volt_set_warnings(volt_io)

add_executable(volt_app
  app/main.cpp
)

target_link_libraries(volt_app PRIVATE volt_core volt_platform volt_render volt_io)
volt_set_warnings(volt_app)
