// What the schematic view's shaders share. See <app/schematic/renderer.hpp>.

#ifndef VOLT_SCHEMATIC_GLSL
#define VOLT_SCHEMATIC_GLSL

// volt::schematic::renderer's pass_constants
layout(push_constant) uniform pass_constants {
  vec2 viewport;   // size of the target in pixels
  vec2 center;     // the world position in the middle of it
  float zoom;      // pixels per world unit
  float grid;      // spacing of the grid in world units
  int values;      // where the simulation's values start, -1 for none
  float low;       // the values that map to the two ends of the ramp
  float high;
  vec4 grid_color; // working space, alpha not multiplied in
} pass;

// World y points up, pixel y points down.
vec2 world_to_pixels(vec2 world) {
  return (world - pass.center) * vec2(pass.zoom, -pass.zoom) + pass.viewport * 0.5;
}

vec2 pixels_to_world(vec2 pixels) {
  return (pixels - pass.viewport * 0.5) / vec2(pass.zoom, -pass.zoom) + pass.center;
}

#endif
