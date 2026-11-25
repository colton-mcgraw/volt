
# -------------------------------------------------------------
# Core

# Header-only for now. Once volt_core gains .cpp files, drop INTERFACE, list the
# sources, switch the keywords below to PUBLIC and call volt_set_warnings(volt_core).
add_library(volt_core INTERFACE)

target_include_directories(volt_core INTERFACE ${PROJECT_SOURCE_DIR}/volt/include)
target_compile_features(volt_core INTERFACE cxx_std_23)

# -------------------------------------------------------------
# Platform

# One backend per OS behind the portable <platform/platform.hpp> interface.
# Backend headers (<platform/windows/...>) are only usable on their own OS.
add_library(volt_platform STATIC)

# What the file dialogs of every OS are built from (<platform/file_dialog.hpp>).
target_sources(volt_platform PRIVATE platform/file_dialog.cpp)

if(WIN32)
  target_sources(volt_platform PRIVATE
    platform/windows/platform.cpp
    platform/windows/window.cpp
    platform/windows/file_dialog.cpp
  )

  # PUBLIC: <platform/windows/win32.hpp> pulls in <windows.h>, so everything that
  # includes it has to agree on these. UNICODE selects the wide-character API
  # (strings cross the boundary as UTF-16), NOMINMAX keeps the min/max macros
  # away from std::min/std::max.
  target_compile_definitions(volt_platform PUBLIC
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
  )

  # gdi32 for the stock background brush, shell32 for file drag and drop, for
  # opening links and for the file dialogs' shell items, shcore for a
  # monitor's scale, ole32 for the COM that opening a link may need and the
  # file dialogs do, uuid for the dialogs' class and interface ids.
  target_link_libraries(volt_platform PRIVATE user32 gdi32 shell32 shcore ole32 uuid)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(volt_platform PRIVATE
    platform/linux/platform.cpp
    platform/linux/input.cpp
    platform/linux/window.cpp
    platform/linux/file_dialog.cpp
  )

  # Debian/Ubuntu: libwayland-dev wayland-protocols libxkbcommon-dev
  # Fedora:        wayland-devel wayland-protocols-devel libxkbcommon-devel
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
  pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
  pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
  pkg_check_modules(WAYLAND_SCANNER REQUIRED wayland-scanner)
  pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
  pkg_get_variable(WAYLAND_SCANNER_EXECUTABLE wayland-scanner wayland_scanner)

  # Core Wayland only knows surfaces. What makes one a desktop window is
  # defined in protocol extensions, published as XML: wayland-scanner turns
  # each into a header and a small C file describing its messages. That glue is
  # C that does not compile as C++, hence its own library and the C compiler.
  enable_language(C)
  add_library(volt_wayland_protocols STATIC)

  set(protocol_dir ${CMAKE_CURRENT_BINARY_DIR}/wayland-protocols)
  file(MAKE_DIRECTORY ${protocol_dir})

  foreach(protocol IN ITEMS
      stable/xdg-shell/xdg-shell                              # windows
      unstable/xdg-decoration/xdg-decoration-unstable-v1)     # title bars drawn by the compositor
    get_filename_component(protocol_name ${protocol} NAME)
    set(protocol_xml ${WAYLAND_PROTOCOLS_DIR}/${protocol}.xml)
    set(protocol_header ${protocol_dir}/${protocol_name}-client-protocol.h)
    set(protocol_code ${protocol_dir}/${protocol_name}-protocol.c)

    add_custom_command(
      OUTPUT ${protocol_header} ${protocol_code}
      COMMAND ${WAYLAND_SCANNER_EXECUTABLE} client-header ${protocol_xml} ${protocol_header}
      COMMAND ${WAYLAND_SCANNER_EXECUTABLE} private-code ${protocol_xml} ${protocol_code}
      DEPENDS ${protocol_xml}
      COMMENT "Generating Wayland protocol ${protocol_name}"
      VERBATIM)
    target_sources(volt_wayland_protocols PRIVATE ${protocol_header} ${protocol_code})
  endforeach()

  # SYSTEM: the generated headers are C and trip the C++ warnings.
  target_include_directories(volt_wayland_protocols SYSTEM PUBLIC ${protocol_dir})
  target_link_libraries(volt_wayland_protocols PUBLIC PkgConfig::WAYLAND_CLIENT)

  # PUBLIC: <platform/linux/wayland.hpp> includes <wayland-client.h>. The
  # generated headers and xkbcommon stay inside the backend.
  target_link_libraries(volt_platform
    PUBLIC PkgConfig::WAYLAND_CLIENT
    PRIVATE PkgConfig::XKBCOMMON volt_wayland_protocols)
else()
  message(FATAL_ERROR
    "Volt has no platform backend for ${CMAKE_SYSTEM_NAME} yet; "
    "only Windows and Linux are implemented (volt/src/platform).")
endif()

target_link_libraries(volt_platform PUBLIC volt_core)
volt_set_warnings(volt_platform)

# -------------------------------------------------------------
# GPU

# The Vulkan device and the frame loop, shared by every renderer and by the
# simulation's compute work. Only gpu/<os>/surface.cpp touches the window
# system, which is why volt_platform stays a private dependency.
add_library(volt_gpu STATIC
  gpu/vulkan.cpp
  gpu/context.cpp
  gpu/swapchain.cpp
  gpu/presenter.cpp
  gpu/buffer.cpp
  gpu/target.cpp
  gpu/texture.cpp
  gpu/pipeline.cpp
  gpu/retire.cpp
)

if(WIN32)
  target_sources(volt_gpu PRIVATE gpu/windows/surface.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(volt_gpu PRIVATE gpu/linux/surface.cpp)
endif()

target_link_libraries(volt_gpu
  PUBLIC volt_core Vulkan::Vulkan
  PRIVATE volt_platform)
volt_set_warnings(volt_gpu)

# -------------------------------------------------------------
# Images, fonts, models

# Each is a library of its own that knows nothing about the GPU or the
# window: files in, plain data out. Their directories hold their sources and
# say how they are built.
add_subdirectory(image)
add_subdirectory(font)
add_subdirectory(model)

# JSON, and on it the parts library, the catalog and the project file.
add_subdirectory(json)
add_subdirectory(library)

# Change history: undo and redo, and a project's snapshots and branches.
add_subdirectory(history)

# The schematic document model: sheets, symbols, editing with undo, files.
add_subdirectory(schematic)

# -------------------------------------------------------------
# Interface

# The widget framework, free of the GPU and the window; see its directory.
add_subdirectory(ui)

# -------------------------------------------------------------
# App

add_executable(volt_app
  main.cpp
  app/ui/renderer.cpp
  app/ui/glyph_cache.cpp
  app/ui/images.cpp
  app/ui/documents.cpp
  app/ui/schematic_document.cpp
  app/ui/services.cpp
  app/ui/services_dialogs.cpp
  app/ui/project_history.cpp
  app/ui/file_dialogs.cpp
  app/ui/window_manager.cpp
  app/ui/windows/content.cpp
  app/ui/windows/menus.cpp
  app/ui/windows/status_bar.cpp
  app/ui/windows/about.cpp
  app/ui/windows/preferences.cpp
  app/ui/windows/image_viewer.cpp
  app/ui/windows/project_manager.cpp
  app/ui/windows/schematic_editor.cpp
  app/ui/windows/schematic_tools.cpp
  app/ui/windows/schematic_view.cpp
  app/ui/windows/schematic_properties.cpp
  app/ui/windows/board_editor.cpp
  app/ui/windows/board_view.cpp
  app/ui/windows/library_browser.cpp
  app/ui/windows/parts_panel.cpp
  app/ui/windows/library_model.cpp
  app/ui/windows/part_match.cpp
  app/ui/windows/library_dialogs.cpp
  app/ui/windows/library_clipboard.cpp
  app/ui/windows/library_widgets.cpp
  app/ui/windows/part_editor.cpp
  app/ui/windows/part_editor_model.cpp
  app/schematic/renderer.cpp
  app/board/renderer.cpp
  app/board/scene.cpp
  app/board/model.cpp
  app/board/model_upload.cpp
  app/simulation/compute.cpp
)

if(WIN32)
  # The application icon (see volt.rc).
  target_sources(volt_app PRIVATE volt.rc)
  set_property(SOURCE volt.rc APPEND PROPERTY OBJECT_DEPENDS ${PROJECT_SOURCE_DIR}/assets/images/volt.ico)
endif()

target_link_libraries(volt_app PRIVATE volt_core volt_platform volt_gpu volt_ui volt_font volt_image volt_model volt_library volt_history volt_schematic)
volt_set_warnings(volt_app)

# Shaders are compiled to SPIR-V with the build and become part of the
# executable: glslc writes each as the initializer of an array of words, and
# the renderer that uses it includes that as <shaders/NAME.inc>. So there is
# nothing to find at run time, and no shader that can be out of step with the
# code that feeds it.
set(shader_source_dir ${PROJECT_SOURCE_DIR}/assets/shaders)
set(shader_binary_dir ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${shader_binary_dir}/shaders)

# What the shaders #include: a change there rebuilds all of them.
set(shader_includes
  ${shader_source_dir}/color.glsl
  ${shader_source_dir}/ui.glsl
  ${shader_source_dir}/schematic.glsl
  ${shader_source_dir}/board.glsl
)

foreach(shader IN ITEMS
    ui_quad.vert
    ui_quad.frag
    schematic_grid.vert
    schematic_grid.frag
    schematic_segment.vert
    schematic_segment.frag
    board_box.vert
    board_box.frag
    board_mesh.vert
    board_mesh.frag
    simulation_relax.comp)
  set(shader_output ${shader_binary_dir}/shaders/${shader}.inc)
  add_custom_command(
    OUTPUT ${shader_output}
    COMMAND Vulkan::glslc -mfmt=c -I ${shader_source_dir} ${shader_source_dir}/${shader} -o ${shader_output}
    DEPENDS ${shader_source_dir}/${shader} ${shader_includes}
    COMMENT "Compiling shader ${shader}"
    VERBATIM)
  target_sources(volt_app PRIVATE ${shader_output})
endforeach()

target_include_directories(volt_app PRIVATE ${shader_binary_dir})

add_custom_command(TARGET volt_app POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:volt_app>/assets/images
  COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:volt_app>/assets/ui
  COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:volt_app>/assets/fonts
  COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:volt_app>/assets/models
  COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:volt_app>/assets/libraries
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PROJECT_SOURCE_DIR}/assets/manifest.json $<TARGET_FILE_DIR:volt_app>/assets/manifest.json
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_SOURCE_DIR}/assets/images $<TARGET_FILE_DIR:volt_app>/assets/images
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_SOURCE_DIR}/assets/ui $<TARGET_FILE_DIR:volt_app>/assets/ui
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_SOURCE_DIR}/assets/fonts $<TARGET_FILE_DIR:volt_app>/assets/fonts
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_SOURCE_DIR}/assets/models $<TARGET_FILE_DIR:volt_app>/assets/models
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_SOURCE_DIR}/assets/libraries $<TARGET_FILE_DIR:volt_app>/assets/libraries
  COMMENT "Staging runtime assets")
