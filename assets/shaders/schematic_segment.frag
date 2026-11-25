#version 450

layout(location = 0) in vec2 in_pixel;
layout(location = 1) flat in vec4 in_ends;
layout(location = 2) flat in float in_radius;
layout(location = 3) flat in vec4 in_color;

layout(location = 0) out vec4 out_color;

void main() {
  // Distance to the line between the two ends.
  vec2 along = in_ends.zw - in_ends.xy;
  vec2 offset = in_pixel - in_ends.xy;
  float t = clamp(dot(offset, along) / max(dot(along, along), 1e-12), 0.0, 1.0);
  float distance = length(offset - along * t) - in_radius;

  float alpha = in_color.a * clamp(0.5 - distance, 0.0, 1.0);
  out_color = vec4(in_color.rgb * alpha, alpha);
}
