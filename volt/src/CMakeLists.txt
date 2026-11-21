
# -------------------------------------------------------------
# Core

# Header-only for now. Once volt_core gains .cpp files, drop INTERFACE, list the
# sources, switch the keywords below to PUBLIC and call volt_set_warnings(volt_core).
add_library(volt_core INTERFACE)

target_include_directories(volt_core INTERFACE ${PROJECT_SOURCE_DIR}/volt/include)
target_compile_features(volt_core INTERFACE cxx_std_23)

# -------------------------------------------------------------
# App

add_executable(volt_app
  main.cpp
)

target_link_libraries(volt_app PRIVATE volt_core Vulkan::Vulkan)
volt_set_warnings(volt_app)

if(TARGET Vulkan::glslc)
  add_custom_command(TARGET volt_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:volt_app>/assets/shaders
    COMMAND Vulkan::glslc ${PROJECT_SOURCE_DIR}/assets/shaders/scaffold.vert -o $<TARGET_FILE_DIR:volt_app>/assets/shaders/scaffold.vert.spv
    COMMAND Vulkan::glslc ${PROJECT_SOURCE_DIR}/assets/shaders/scaffold.frag -o $<TARGET_FILE_DIR:volt_app>/assets/shaders/scaffold.frag.spv
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
