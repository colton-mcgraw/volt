#version 450
#extension GL_GOOGLE_include_directive : require
#include "board.glsl"

// One instance per placement of a mesh (volt::board::mesh_instance), the
// mesh's vertices from binding 1 (volt::board::vertex).
layout(location = 0) in vec4 in_model_0; // the columns of the model matrix
layout(location = 1) in vec4 in_model_1;
layout(location = 2) in vec4 in_model_2;
layout(location = 3) in vec4 in_model_3;
layout(location = 4) in vec4 in_color; // rgb: working space; a: -1 where the transform mirrors, else 1
layout(location = 5) in vec3 in_position;
layout(location = 6) in vec3 in_normal;

layout(location = 0) out vec3 out_normal;
layout(location = 1) out vec3 out_position; // world space
layout(location = 2) flat out vec4 out_color;

void main() {
  mat4 model = mat4(in_model_0, in_model_1, in_model_2, in_model_3);
  vec4 world = model * vec4(in_position, 1.0);

  // Parts are placed by rigid motions, at most with a uniform scale or a
  // mirror (volt::board::mesh_instance). Those keep a normal perpendicular
  // to its surface, so the matrix itself turns it; the fragment shader
  // normalizes away the scale.
  out_normal = mat3(model) * in_normal;
  out_position = world.xyz;
  out_color = in_color;
  gl_Position = pass.view_projection * world;
}
