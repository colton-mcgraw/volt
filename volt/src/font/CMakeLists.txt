# Fonts from scratch: sfnt parsing (TrueType and CFF outlines, collections),
# outlines as geometry, coverage and distance field rasterization, a glyph
# atlas and simple left-to-right text layout. Bytes in, plain data out: no
# GPU, no window.
add_library(volt_font STATIC
  sfnt.cpp
  cmap.cpp
  glyf.cpp
  cff.cpp
  kerning.cpp
  outline.cpp
  utf8.cpp
  raster.cpp
  sdf.cpp
  text.cpp
  atlas.cpp
)

target_link_libraries(volt_font PUBLIC volt_core)
volt_set_warnings(volt_font)
