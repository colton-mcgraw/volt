# Tests of the ui module.
#
# Input is tested without a window: the processor is fed raw callback-level
# input with a hand-driven clock, and window_input is attached to a stand-in
# that has the window's setters.
volt_add_test(volt_ui_input_processor_tests ui/input_processor_tests.cpp)
volt_add_test(volt_ui_window_input_tests ui/window_input_tests.cpp)
volt_add_test(volt_ui_shortcut_tests ui/shortcut_tests.cpp)

# The widget framework, without a GPU: a screen with the repository's font,
# driven through the real input processor (harness.hpp). They read only the
# repository's fonts, found through VOLT_ASSETS_DIR.
foreach(suite IN ITEMS draw_list layout routing text_field popup view workspace column_view)
  volt_add_test(volt_ui_${suite}_tests ui/${suite}_tests.cpp)
  target_link_libraries(volt_ui_${suite}_tests PRIVATE volt_ui volt_font)
  target_compile_definitions(volt_ui_${suite}_tests PRIVATE VOLT_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets")
endforeach()

# The toggle list (the board editor's objects, any list of things to show or
# hide), the same way.
volt_add_test(volt_ui_toggle_list_tests ui/toggle_list_tests.cpp)
target_link_libraries(volt_ui_toggle_list_tests PRIVATE volt_ui volt_font)
target_compile_definitions(volt_ui_toggle_list_tests PRIVATE VOLT_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets")

# The application's pieces that need no GPU: the image index space, and how a
# loaded model becomes the board's meshes and instances (its source files
# compiled in, since the application is an executable).
volt_add_test(volt_app_tests app/app_tests.cpp ${PROJECT_SOURCE_DIR}/volt/src/app/board/model.cpp
  ${PROJECT_SOURCE_DIR}/volt/src/app/board/scene.cpp)
target_link_libraries(volt_app_tests PRIVATE volt_model)

# The demo board's part models: the committed STEP files are what
# app/demo_parts.hpp writes, and load clean.
volt_add_test(volt_app_demo_parts_tests app/demo_parts_tests.cpp)
target_link_libraries(volt_app_demo_parts_tests PRIVATE volt_model)
target_compile_definitions(volt_app_demo_parts_tests PRIVATE VOLT_MODEL_ASSETS="${PROJECT_SOURCE_DIR}/assets/models")
