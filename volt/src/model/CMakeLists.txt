# volt_model: 3D models in, plain data out, and back. STEP (ISO 10303-21
# files of AP203, AP214 and AP242), Wavefront OBJ and STL, all read and
# written here without third-party code. Knows nothing about the GPU.
add_library(volt_model STATIC
  model.cpp
  measure.cpp
  options.cpp

  # The Part 21 physical file format.
  part21_file.cpp
  part21_reader.cpp
  part21_writer.cpp

  # Geometry: curves, surfaces, and how a trimmed face becomes triangles.
  bspline.cpp
  curves.cpp
  surfaces.cpp
  triangulate.cpp
  face_tessellator.cpp

  # STEP: entity access, shapes and colors, product structure, writing.
  step_access.cpp
  step_shapes.cpp
  step_reader.cpp
  step_writer.cpp

  # The simple formats, and the front door that picks a format.
  obj.cpp
  stl.cpp
  io.cpp
)

target_link_libraries(volt_model PUBLIC volt_core)
volt_set_warnings(volt_model)
