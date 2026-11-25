#version 450
#extension GL_GOOGLE_include_directive : require
#include "schematic.glsl"

layout(location = 0) out vec4 out_color;

// How much of the pixel the lines of a grid with this spacing cover. Lines
// are one pixel wide, and fade out before they get close enough together
// to turn the view gray.
float lines(vec2 world, float spacing) {
  float pixels = spacing * pass.zoom;
  vec2 distance = abs(fract(world / spacing + 0.5) - 0.5) * pixels;
  float coverage = clamp(1.0 - min(distance.x, distance.y), 0.0, 1.0);
  return coverage * clamp((pixels - 4.0) / 8.0, 0.0, 1.0);
}

void main() {
  vec2 world = pixels_to_world(gl_FragCoord.xy);

  // Every tenth line is a major one.
  float coverage = max(lines(world, pass.grid) * 0.5, lines(world, pass.grid * 10.0));
  float alpha = pass.grid_color.a * coverage;
  out_color = vec4(pass.grid_color.rgb * alpha, alpha);
}
