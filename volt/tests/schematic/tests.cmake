# Tests of the schematic module: the sheet and its changes, the built-in
# and generated symbols, every operation of the edited document with undo
# and redo (merging, transactions, a long random run), wires that follow
# moved parts, wires cut by parts dropped on them and scoped tidying, the
# .voltsch file, and the demo sheet.
foreach(suite IN ITEMS sheet symbol document follow io demo)
  volt_add_test(volt_schematic_${suite}_tests schematic/${suite}_tests.cpp)
  target_link_libraries(volt_schematic_${suite}_tests PRIVATE volt_schematic)
endforeach()

# Drawing and hit testing, and the netlist: each lists its own tests.
include(schematic/draw_tests.cmake)
include(schematic/netlist_tests.cmake)
