#version 450
#extension GL_GOOGLE_include_directive : require
#include "board.glsl"

layout(location = 0) in vec3 in_normal;
layout(location = 1) in vec3 in_position;
layout(location = 2) flat in vec4 in_color;

layout(location = 0) out vec4 out_color;

void main() {
  // The box's own faces are wound both ways (board_box.vert), so which side
  // is in front comes from the normal, which always points out. A mark
  // (the selection's outline, a measurement) keeps its colour as it is,
  // so that it reads the same from every side. Drawn again where it is
  // behind something (a = 3, blended, premultiplied), it shows through
  // faintly.
  if (in_color.a > 2.5)
    out_color = vec4(in_color.rgb * 0.45, 0.45);
  else if (in_color.a > 1.5)
    out_color = vec4(in_color.rgb, 1.0);
  else
    out_color = vec4(shade(in_color.rgb, normalize(in_normal), in_position), 1.0);
}
