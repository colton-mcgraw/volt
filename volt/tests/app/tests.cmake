# Tests of the application's window architecture that need no window and no
# GPU: the interface scale, where windows open and how small they get, their
# titles, the window book (which kinds are open, what a request means), and
# painting several windows against one glyph atlas.
# (The older app tests are registered in ui/tests.cmake.)
volt_add_test(volt_app_windows_tests app/windows_tests.cpp)
target_link_libraries(volt_app_windows_tests PRIVATE volt_ui volt_font volt_platform)

# The board editor's logic that needs no GPU: the board scene's objects,
# picking and projection, and the view controller (their sources compiled in).
volt_add_test(volt_app_board_tests app/board_tests.cpp
  ${PROJECT_SOURCE_DIR}/volt/src/app/board/scene.cpp
  ${PROJECT_SOURCE_DIR}/volt/src/app/board/model.cpp
  ${PROJECT_SOURCE_DIR}/volt/src/app/ui/windows/board_view.cpp)
target_link_libraries(volt_app_board_tests PRIVATE volt_model volt_ui volt_font)

# The log of the windows' status messages that the project manager lists.
volt_add_test(volt_app_message_log_tests app/message_log_tests.cpp)
target_link_libraries(volt_app_message_log_tests PRIVATE volt_ui)

# What a window about to close, the application about to exit or the project
# about to go asks first of the work it holds (app/ui/close_guards.hpp).
volt_add_test(volt_app_close_guards_tests app/close_guards_tests.cpp)

# The application's file dialogs (app/ui/file_dialogs.cpp, compiled in): what
# each purpose asks for, and that File > Open lists what a drop opens.
volt_add_test(volt_app_file_dialogs_tests app/file_dialogs_tests.cpp
  ${PROJECT_SOURCE_DIR}/volt/src/app/ui/file_dialogs.cpp)
target_link_libraries(volt_app_file_dialogs_tests PRIVATE volt_platform volt_library volt_model volt_image)

# The library windows' model (app/ui/windows/library_model.cpp, compiled
# in): the parts list from a catalog, filter and sort, the selection found
# again after a rescan, the scope tree, the project library's status rows.
volt_add_test(volt_app_library_model_tests app/library_model_tests.cpp
  ${PROJECT_SOURCE_DIR}/volt/src/app/ui/windows/library_model.cpp)
target_link_libraries(volt_app_library_model_tests PRIVATE volt_library)

# Placeholders and the parts that stand in for them (app/ui/windows/
# part_match.cpp, compiled in): the Generic scope's entries and search,
# which placed parts are placeholders, the ranked candidates for one, and a
# part assigned.
volt_add_test(volt_app_part_match_tests app/part_match_tests.cpp
  ${PROJECT_SOURCE_DIR}/volt/src/app/ui/windows/part_match.cpp)
target_link_libraries(volt_app_part_match_tests PRIVATE volt_library volt_schematic)

# The part editor's model (app/ui/windows/part_editor_model.cpp, compiled
# in): fields, the id a part is given when saved and keeps, parameters and
# their completion from the schema, the live checks, a device's parameters,
# and committing new, edited and duplicated parts (with their model files)
# into a library.
volt_add_test(volt_app_part_editor_model_tests app/part_editor_model_tests.cpp
  ${PROJECT_SOURCE_DIR}/volt/src/app/ui/windows/part_editor_model.cpp)
target_link_libraries(volt_app_part_editor_model_tests PRIVATE volt_library)

# The open project's history (app/ui/project_history.cpp, compiled in): the
# files a project tracks, snapshots, the status on disk, branches made,
# switched, renamed and deleted, old snapshots restored, failures that leave
# the head where it was.
volt_add_test(volt_app_project_history_tests app/project_history_tests.cpp
  ${PROJECT_SOURCE_DIR}/volt/src/app/ui/project_history.cpp)
target_link_libraries(volt_app_project_history_tests PRIVATE volt_library volt_history)

# The application's schematic document (app/ui/schematic_document.cpp,
# compiled in): the scratch sheet and a project's sheets, the scene rebuilt
# from the active one, the signals, the simulation's nodes shown only while
# current, modified state, and a project's sheets loaded and saved.
volt_add_test(volt_app_schematic_document_tests app/schematic_document_tests.cpp
  ${PROJECT_SOURCE_DIR}/volt/src/app/ui/schematic_document.cpp)
target_link_libraries(volt_app_schematic_document_tests PRIVATE volt_schematic volt_library volt_ui volt_history)

# The schematic editor's tools list their own tests.
include(app/schematic_tools_tests.cmake)

# The schematic editor's view controller and its selection properties page
# (app/ui/windows/schematic_view.cpp and schematic_properties.cpp, with the
# tools and part_match.cpp compiled in): events to the tools or the camera,
# the tolerance, right clicks and the context menu, through the real
# viewport and input processor; the page on a headless screen, typing as
# undoable steps, the keyboard handed to a label's text and back to the
# canvas, the Tool section.
volt_add_test(volt_app_schematic_editor_tests app/schematic_editor_tests.cpp
  ${PROJECT_SOURCE_DIR}/volt/src/app/ui/windows/schematic_view.cpp
  ${PROJECT_SOURCE_DIR}/volt/src/app/ui/windows/schematic_properties.cpp
  ${PROJECT_SOURCE_DIR}/volt/src/app/ui/windows/schematic_tools.cpp
  ${PROJECT_SOURCE_DIR}/volt/src/app/ui/windows/part_match.cpp)
target_link_libraries(volt_app_schematic_editor_tests PRIVATE volt_schematic volt_library volt_ui volt_font)
target_compile_definitions(volt_app_schematic_editor_tests PRIVATE VOLT_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets")
