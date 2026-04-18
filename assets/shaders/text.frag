#version 450

layout(location = 0) in  vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D msdfAtlas;

layout(push_constant) uniform PushConstants {
    vec4 color;
    float pxRange;   // screen pixels per SDF unit — scale with glyph size
};

float median(float r, float g, float b) {
    return max(min(r, g), min(max(r, g), b));
}

void main() {
    vec4 s       = texture(msdfAtlas, fragUV);
    float dist   = median(s.r, s.g, s.b) - 0.5;

    // Convert SDF distance to screen-space AA width
    float width  = fwidth(dist) * 0.5;
    float alpha  = smoothstep(-width, width, dist);

    outColor = vec4(color.rgb, color.a * alpha);
}