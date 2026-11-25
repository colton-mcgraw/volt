# -------------------------------------------------------------
# Images

# Pixels in memory and the codecs that fill them: PNG, JPEG, BMP and SVG,
# with the DEFLATE implementation PNG needs (also used by fonts and models).
# Written from scratch, like the rest of Volt; no GPU, no window.
add_library(volt_image STATIC
  image.cpp
  compress.cpp
  inflate.cpp
  deflate.cpp
  png.cpp
  jpeg_decode.cpp
  jpeg_encode.cpp
  bmp.cpp
  xml.cpp
  path.cpp
  stroke.cpp
  raster.cpp
  svg_style.cpp
  svg_parse.cpp
  svg_render.cpp
  io.cpp
)

target_link_libraries(volt_image PUBLIC volt_core)
volt_set_warnings(volt_image)
