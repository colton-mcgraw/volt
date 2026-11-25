# Tests of the netlist and the network a simulation solves.
#
# The nets of a sheet: every connectivity rule and its negative, parts in
# all orientations, names from labels and power ports, the problems,
# determinism, the demo ladder, and a large sheet in good time. Then the
# circuit: the ladder, a divider, sources, values, inductors, what is left
# out, solved on the CPU.
foreach(suite IN ITEMS netlist circuit)
  volt_add_test(volt_schematic_${suite}_tests schematic/${suite}_tests.cpp)
  target_link_libraries(volt_schematic_${suite}_tests PRIVATE volt_schematic)
endforeach()
