# volt_schematic: the schematic document model. Sheets of parts, wires and
# labels (<schematic/sheet.hpp>), the built-in symbols that draw parts and
# place their pins (<schematic/symbol.hpp>), the edited document with its
# undo history (<schematic/document.hpp>, on <history/record.hpp>) and the
# wires that follow what it moves (<schematic/follow.hpp>), the
# .voltsch file (<schematic/io.hpp>), a demo sheet, the sheet as the lines
# the renderer draws (<schematic/draw.hpp>, text by <schematic/stroke_font.hpp>),
# hit testing (<schematic/hit.hpp>) and the nets and the network a
# simulation solves (<schematic/netlist.hpp>). Plain data: no GPU,
# no window, no widgets; whatever draws, netlists or edits a sheet builds on
# it.
add_library(volt_schematic STATIC
  sheet.cpp
  symbol.cpp
  document.cpp
  follow.cpp
  io.cpp
  demo.cpp
  stroke_font.cpp
  draw.cpp
  hit.cpp
  netlist.cpp
)

target_link_libraries(volt_schematic PUBLIC volt_core volt_json)
volt_set_warnings(volt_schematic)
