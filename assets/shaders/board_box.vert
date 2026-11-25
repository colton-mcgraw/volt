#version 450
#extension GL_GOOGLE_include_directive : require
#include "board.glsl"

// One instance per box (volt::board::box): the unit cube, centered on the
// origin, under the instance's transform. The cube's 36 vertices come from
// the vertex index.
layout(location = 0) in vec4 in_model_0; // the columns of the model matrix
layout(location = 1) in vec4 in_model_1;
layout(location = 2) in vec4 in_model_2;
layout(location = 3) in vec4 in_model_3;
layout(location = 4) in vec4 in_color; // rgb: working space; a: 1 lit, 2 a mark, drawn unlit,
                                       // 3 a mark where something is in front of it

layout(location = 0) out vec3 out_normal;
layout(location = 1) out vec3 out_position; // world space
layout(location = 2) flat out vec4 out_color;

void main() {
  // Per pair of faces: the axis they face along, and the two that span them.
  const ivec3 axes[3] = ivec3[3](ivec3(0, 1, 2), ivec3(1, 2, 0), ivec3(2, 0, 1));
  const vec2 corners[6] = vec2[6](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));

  int face = gl_VertexIndex / 6;
  ivec3 axis = axes[face >> 1];
  float side = (face & 1) == 0 ? 1.0 : -1.0;
  vec2 corner = corners[gl_VertexIndex % 6];

  vec3 position = vec3(0.0);
  position[axis.x] = side * 0.5;
  position[axis.y] = corner.x * 0.5;
  position[axis.z] = corner.y * 0.5;

  vec3 normal = vec3(0.0);
  normal[axis.x] = side;

  mat4 model = mat4(in_model_0, in_model_1, in_model_2, in_model_3);
  vec4 world = model * vec4(position, 1.0);

  // A box is only ever scaled along its own axes, which leaves the
  // directions of its face normals alone: no inverse transpose needed.
  out_normal = mat3(model) * normal;
  out_position = world.xyz;
  out_color = in_color;
  gl_Position = pass.view_projection * world;
}
