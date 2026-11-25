# Tests of drawing a sheet and of hit testing.
#
# The stroke font: every glyph inside its box, widths, alignment and the
# four turns, UTF-8 good and bad. Drawing: junction dots exactly where wires
# join, the demo ladder (nodes, every part's strokes and text, bounds), the
# selection, marks on free pins, labels, the overlays. Hit testing: every
# kind in every turn, priority, tolerances, rectangles, wire ends, snapping,
# boxes.

foreach(suite IN ITEMS stroke_font draw hit)
  volt_add_test(volt_schematic_${suite}_tests schematic/${suite}_tests.cpp)
  target_link_libraries(volt_schematic_${suite}_tests PRIVATE volt_schematic)
endforeach()
