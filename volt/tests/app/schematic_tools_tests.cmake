# Tests of the schematic editor's tools (app/ui/windows/schematic_tools.cpp,
# compiled in): selecting, dragging (with the wires following), rubber bands,
# drawing wires, placing parts and labels (cutting the wires they land on),
# the editing commands and their undo, driven as scripted
# pointer and key events against real documents, and a long random run of
# events that must never break the document or the selection.
volt_add_test(volt_app_schematic_tools_tests app/schematic_tools_tests.cpp
  ${PROJECT_SOURCE_DIR}/volt/src/app/ui/windows/schematic_tools.cpp)
target_link_libraries(volt_app_schematic_tools_tests PRIVATE volt_schematic)
