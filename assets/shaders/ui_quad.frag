#version 450
#extension GL_GOOGLE_include_directive : require
#include "ui.glsl"

// Every shape of the interface (volt::ui::shape_kind), from one pipeline,
// so that a list switches pipelines never and descriptor sets only where it
// moves to another texture.
layout(location = 0) flat in vec4 in_rect;
layout(location = 1) flat in vec4 in_color;
layout(location = 2) flat in vec4 in_border_color;
layout(location = 3) flat in vec4 in_uv;
layout(location = 4) flat in vec4 in_params;

// A page of the glyph atlas (coverage, linear 8 bit) and an image, both
// always bound; a default is bound where an item has none.
layout(set = 0, binding = 0) uniform sampler2D glyphs;
layout(set = 0, binding = 1) uniform sampler2D image;

layout(location = 0) out vec4 out_color;

vec2 texture_coordinate(vec2 pixel) {
  vec2 t = clamp((pixel - in_rect.xy) / max(in_rect.zw, vec2(0.0001)), 0.0, 1.0);
  return mix(in_uv.xy, in_uv.zw, t);
}

void main() {
  vec2 pixel = gl_FragCoord.xy;
  int kind = int(in_params.w + 0.5);
  float radius = in_params.x;

  if (kind == SHAPE_LINE) {
    vec2 a = in_rect.xy;
    vec2 b = in_rect.zw;
    vec2 ab = b - a;
    float t = clamp(dot(pixel - a, ab) / max(dot(ab, ab), 0.0001), 0.0, 1.0);
    float distance = length(pixel - (a + ab * t)) - in_params.z * 0.5;
    out_color = window_color(in_color.rgb, in_color.a * clamp(0.5 - distance, 0.0, 1.0));
    return;
  }

  vec2 half_size = in_rect.zw * 0.5;
  vec2 point = pixel - (in_rect.xy + half_size);

  if (kind == SHAPE_GLYPH) {
    float coverage = texture(glyphs, texture_coordinate(pixel)).r;
    // Heavier coverage for text that blending would leave thin.
    coverage = pow(coverage, 1.0 / (1.0 + in_params.z));
    out_color = window_color(in_color.rgb, in_color.a * coverage);
    return;
  }

  if (kind == SHAPE_SHADOW) {
    float blur = max(in_params.z, 0.5);
    float distance = rounded_distance(point, half_size, radius);
    float strength = 1.0 - smoothstep(-blur, blur, distance);
    out_color = window_color(in_color.rgb, in_color.a * strength * strength);
    return;
  }

  float coverage = rounded_coverage(point, half_size, radius);

  if (kind == SHAPE_IMAGE) {
    // Premultiplied in linear light (<app/ui/renderer.hpp>), so filtered
    // right; window_color wants the colour without its alpha.
    vec4 texel = texture(image, texture_coordinate(pixel));
    vec3 linear = texel.a > 0.0 ? texel.rgb / texel.a : vec3(0.0);
    out_color = window_color(linear * in_color.rgb, texel.a * in_color.a * coverage);
    return;
  }

  // A rectangle, filled, with its border inside the edge. The border's
  // inner edge is a rounded rectangle of its own, smaller by the border.
  float border = in_params.y;
  vec4 fill = vec4(in_color.rgb * in_color.a, in_color.a);
  if (border > 0.0) {
    float inner = rounded_coverage(point, half_size - border, max(radius - border, 0.0));
    vec4 edge = vec4(in_border_color.rgb * in_border_color.a, in_border_color.a);
    fill = mix(edge, fill, inner);
  }

  float alpha = fill.a * coverage;
  vec3 linear = fill.a > 0.0 ? fill.rgb / fill.a : vec3(0.0);
  out_color = window_color(linear, alpha);
}
