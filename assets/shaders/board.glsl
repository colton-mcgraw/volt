// What the board view's shaders share. See <app/board/renderer.hpp>.

#ifndef VOLT_BOARD_GLSL
#define VOLT_BOARD_GLSL

// volt::board::renderer's pass_constants
layout(push_constant) uniform pass_constants {
  mat4 view_projection;
  vec4 light; // xyz: the direction the light comes from, in world space
  vec4 eye;   // xyz: where the camera is, in world space
} pass;

// The colour of a surface of colour `base` (working space) with unit
// normal `normal` at `position` (world space), facing the viewer.
//
// One light and some of it from everywhere, so that no face goes black. The
// light from everywhere is a hemisphere: more from above (the room) than
// from below (the board), so that faces turned away from the light still
// show which way they point. A soft highlight, the same white on every
// material, lets curved surfaces read as curved; it is small and broad, so
// that nothing looks wet.
vec3 shade(vec3 base, vec3 normal, vec3 position) {
  vec3 l = normalize(pass.light.xyz);
  vec3 v = normalize(pass.eye.xyz - position);

  float diffuse = max(dot(normal, l), 0.0);
  float ambient = mix(0.18, 0.36, normal.z * 0.5 + 0.5);

  vec3 h = normalize(l + v);
  float highlight = diffuse > 0.0 ? 0.10 * pow(max(dot(normal, h), 0.0), 24.0) : 0.0;

  return base * (ambient + 0.64 * diffuse) + vec3(highlight);
}

#endif
