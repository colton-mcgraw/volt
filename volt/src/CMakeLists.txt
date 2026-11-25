
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
