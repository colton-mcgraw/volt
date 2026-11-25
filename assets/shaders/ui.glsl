// What the interface's shaders share. See <app/ui/renderer.hpp> and
// <ui/draw_list.hpp>.

#ifndef VOLT_UI_GLSL
#define VOLT_UI_GLSL

#include "color.glsl"

layout(push_constant) uniform pass_constants {
  vec2 viewport; // size of the window's image in pixels
  int encoding;  // volt::gpu::output
  float white;
} pass;

// volt::ui::shape_kind
const int SHAPE_RECTANGLE = 0;
const int SHAPE_SHADOW = 1;
const int SHAPE_LINE = 2;
const int SHAPE_GLYPH = 3;
const int SHAPE_IMAGE = 4;

// Signed distance from `point` to a rectangle with rounded corners, in
// pixels, negative inside. `point` is relative to the rectangle's center.
float rounded_distance(vec2 point, vec2 half_size, float radius) {
  radius = min(radius, min(half_size.x, half_size.y));
  vec2 q = abs(point) - half_size + radius;
  return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}

// How much of the pixel a rectangle with rounded corners covers, 0 to 1.
float rounded_coverage(vec2 point, vec2 half_size, float radius) {
  return clamp(0.5 - rounded_distance(point, half_size, radius), 0.0, 1.0);
}

// The interface's pass is the one that writes the window, so its colors
// leave encoded for the display, and multiplied by alpha for blending.
//
// The blend therefore happens on encoded values. For scRGB those are
// linear and the result is exact; for sRGB it is the slightly heavier
// edge every desktop interface has; for HDR10 translucent edges come out
// darker than they should.
vec4 window_color(vec3 linear, float alpha) {
  return vec4(encode_output(linear, pass.encoding, pass.white) * alpha, alpha);
}

#endif
