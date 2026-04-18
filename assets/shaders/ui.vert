#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec4 inColor;

layout(push_constant) uniform UiParams {
  mat4 uiTransform;
  float pxRange;
  float edge;
  float aaStrength;
  float msdfMode;
} ui;

layout(location = 0) out vec2 fragUv;
layout(location = 1) out vec4 fragColor;

void main() {
  gl_Position = ui.uiTransform * vec4(inPos, 1.0);
  fragUv = inUv;
  fragColor = inColor;
}