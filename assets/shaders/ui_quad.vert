#version 450
#extension GL_GOOGLE_include_directive : require
#include "ui.glsl"

// One instance per draw list item (volt::ui::draw_item). The four corners
// of the quad that covers it come from the vertex index, as a triangle
// strip; the fragment shader works out coverage from the pixel position.
layout(location = 0) in vec4 in_rect;         // x, y, width, height; a line's two ends
layout(location = 1) in vec4 in_color;        // working space, alpha not multiplied in
layout(location = 2) in vec4 in_border_color; // the same
layout(location = 3) in vec4 in_uv;           // u0, v0, u1, v1
layout(location = 4) in vec4 in_params;       // radius, border, extra, kind

layout(location = 0) flat out vec4 out_rect;
layout(location = 1) flat out vec4 out_color;
layout(location = 2) flat out vec4 out_border_color;
layout(location = 3) flat out vec4 out_uv;
layout(location = 4) flat out vec4 out_params;

void main() {
  vec2 corner = vec2(gl_VertexIndex & 1, gl_VertexIndex >> 1);
  int kind = int(in_params.w + 0.5);
  vec2 position;

  if (kind == SHAPE_LINE) {
    // A quad along the line, half the width and a pixel of edge to each
    // side, and as far past the ends, for the round caps.
    vec2 a = in_rect.xy;
    vec2 b = in_rect.zw;
    vec2 along = b - a;
    float len = length(along);
    vec2 dir = len > 0.0001 ? along / len : vec2(1.0, 0.0);
    vec2 normal = vec2(-dir.y, dir.x);
    float reach = in_params.z * 0.5 + 1.0;
    vec2 start = a - dir * reach;
    vec2 end = b + dir * reach;
    position = mix(start, end, corner.x) + normal * reach * (corner.y * 2.0 - 1.0);
  } else {
    // Glyphs sit on whole pixels at exactly their size; everything else
    // gets room for its antialiased edge, a shadow for its blur.
    float grow = kind == SHAPE_GLYPH ? 0.0 : (kind == SHAPE_SHADOW ? in_params.z * 2.0 : 1.0);
    position = in_rect.xy - grow + corner * (in_rect.zw + 2.0 * grow);
  }

  out_rect = in_rect;
  out_color = in_color;
  out_border_color = in_border_color;
  out_uv = in_uv;
  out_params = in_params;

  // Pixels and Vulkan's clip space both have y pointing down.
  gl_Position = vec4(position / pass.viewport * 2.0 - 1.0, 0.0, 1.0);
}
