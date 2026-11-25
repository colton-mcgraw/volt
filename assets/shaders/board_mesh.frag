#version 450
#extension GL_GOOGLE_include_directive : require
#include "board.glsl"

layout(location = 0) in vec3 in_normal;
layout(location = 1) in vec3 in_position;
layout(location = 2) flat in vec4 in_color;

layout(location = 0) out vec4 out_color;

void main() {
  vec3 normal = normalize(in_normal);

  // Triangles are counter-clockwise seen from outside (model::mesh), so a
  // back face is the inside of the part: one whose file left a face out, or
  // turned one the wrong way. It is lit as if it faced the viewer. A
  // mirroring transform turns every triangle around, which in_color.a says.
  bool outside = gl_FrontFacing == (in_color.a >= 0.0);
  if (!outside)
    normal = -normal;

  out_color = vec4(shade(in_color.rgb, normal, in_position), 1.0);
}
