#version 450
#extension GL_GOOGLE_include_directive : require
#include "schematic.glsl"

// One instance per segment (volt::schematic::segment): a line with round
// ends, which is also a dot when both ends are the same point. The four
// corners of the quad around it come from the vertex index.
layout(location = 0) in vec4 in_ends;  // a.xy, b.xy in world units
layout(location = 1) in vec4 in_color; // working space, alpha not multiplied in
layout(location = 2) in float in_width;
layout(location = 3) in int in_node;

// What the simulation computed, one value per node. Several sets of them
// one after the other: pass.values is where the one to show starts.
layout(set = 0, binding = 0) readonly buffer simulation_values {
  float values[];
} simulation;

layout(location = 0) out vec2 out_pixel;
layout(location = 1) flat out vec4 out_ends; // in pixels
layout(location = 2) flat out float out_radius;
layout(location = 3) flat out vec4 out_color;

// Low to high: blue, through green, to red. Linear, like every color here.
vec3 ramp(float t) {
  t = clamp(t, 0.0, 1.0);
  return clamp(vec3(2.0 * t - 1.0, 1.0 - abs(2.0 * t - 1.0), 1.0 - 2.0 * t), 0.0, 1.0) * 0.9 + 0.05;
}

void main() {
  vec2 a = world_to_pixels(in_ends.xy);
  vec2 b = world_to_pixels(in_ends.zw);

  // Never thinner than a pixel, however far the view is zoomed out.
  float radius = max(in_width * pass.zoom * 0.5, 0.5);

  vec2 along = b - a;
  float span = length(along);
  along = span > 0.0 ? along / span : vec2(1.0, 0.0);
  vec2 across = vec2(-along.y, along.x);

  // A pixel of margin for the antialiased edge.
  float reach = radius + 1.0;
  vec2 corner = vec2(gl_VertexIndex & 1, gl_VertexIndex >> 1);
  vec2 pixel = mix(a - along * reach, b + along * reach, corner.x) + across * reach * (corner.y * 2.0 - 1.0);

  out_pixel = pixel;
  out_ends = vec4(a, b);
  out_radius = radius;
  out_color = in_color;

  if (in_node >= 0 && pass.values >= 0) {
    float value = simulation.values[pass.values + in_node];
    out_color.rgb = ramp((value - pass.low) / max(pass.high - pass.low, 1e-12));
  }

  gl_Position = vec4(pixel / pass.viewport * 2.0 - 1.0, 0.0, 1.0);
}
