#version 450

layout(location = 0) in vec2 fragUv;
layout(location = 1) in vec4 fragColor;

layout(set = 0, binding = 0) uniform sampler2D uiTexture;

layout(push_constant) uniform UiSdfParams {
  mat4 uiTransform;
  float pxRange;
  float edge;
  float aaStrength;
  float msdfMode;
  float msdfConfidenceLow;
  float msdfConfidenceHigh;
  float subpixelBlendStrength;
  float smallTextSharpenStrength;
} sdf;

layout(location = 0) out vec4 outColor;

float median3(vec3 value) {
  return max(min(value.r, value.g), min(max(value.r, value.g), value.b));
}

float sampleSdfDistance(vec2 uv) {
  vec4 texel = texture(uiTexture, uv);
  return texel.a;
}

float sampleSdfAlpha(vec2 uv, float screenPxRange, float aaStrength, float edge) {
  float dist = sampleSdfDistance(uv);
  float smoothing = (0.5 / screenPxRange) * aaStrength;
  return smoothstep(edge - smoothing, edge + smoothing, dist);
}

float sampleMsdfAlpha(vec2 uv, float screenPxRange, float edge, float aaStrength) {
  vec4 texel = texture(uiTexture, uv);
  float msdfSignedDistance = median3(texel.rgb) - 0.5;
  float sdfSignedDistance = texel.a - 0.5;

  float channelSpread = max(
      max(abs(texel.r - texel.g), abs(texel.g - texel.b)),
      abs(texel.r - texel.b));
  float confidenceLow = min(sdf.msdfConfidenceLow, sdf.msdfConfidenceHigh);
  float confidenceHigh = max(sdf.msdfConfidenceLow, sdf.msdfConfidenceHigh);
  float msdfConfidence = smoothstep(confidenceLow, confidenceHigh, channelSpread);

  // Keep alpha SDF as the edge-position anchor and only trust MSDF where it agrees.
  // This avoids glyph-to-glyph weight drift when RGB channels are under-colored or ambiguous.
  float distanceAgreement = abs(msdfSignedDistance - sdfSignedDistance);
  float agreement = 1.0 - smoothstep(0.015, 0.075, distanceAgreement);
  float edgeProximity = 1.0 - smoothstep(0.08, 0.22, abs(sdfSignedDistance));
  float msdfWeight = msdfConfidence * agreement * edgeProximity;
  float signedDistance = mix(sdfSignedDistance, msdfSignedDistance, msdfWeight);

  float aaT = clamp((aaStrength - 0.1) / 0.5, 0.0, 1.0);
  float derivativeWidth = max(fwidth(msdfSignedDistance), 1e-4);
  float pxRangeWidth = 0.5 / max(screenPxRange, 0.75);
  float smoothing = max(
      derivativeWidth * mix(0.55, 0.90, aaT),
      pxRangeWidth * mix(0.45, 0.80, aaT));
  smoothing = clamp(smoothing, 1e-4, 0.25);

  float threshold = clamp(edge, 0.35, 0.65) - 0.5;
  float alpha = smoothstep(threshold - smoothing, threshold + smoothing, signedDistance);
  float sharpen = 1.0 + sdf.smallTextSharpenStrength * (1.0 - smoothstep(1.0, 2.25, screenPxRange));
  return clamp((alpha - 0.5) * sharpen + 0.5, 0.0, 1.0);
}

vec3 sampleSdfCoverageRgb(vec2 uv,
                          vec2 subpixelStep,
                          float screenPxRange,
                          float aaStrength,
                          float edge) {
  return vec3(
      sampleSdfAlpha(uv + subpixelStep, screenPxRange, aaStrength, edge),
      sampleSdfAlpha(uv, screenPxRange, aaStrength, edge),
      sampleSdfAlpha(uv - subpixelStep, screenPxRange, aaStrength, edge));
}

vec3 sampleMsdfCoverageRgb(vec2 uv, vec2 subpixelStep, float screenPxRange, float edge, float aaStrength) {
  return vec3(
      sampleMsdfAlpha(uv + subpixelStep, screenPxRange, edge, aaStrength),
      sampleMsdfAlpha(uv, screenPxRange, edge, aaStrength),
      sampleMsdfAlpha(uv - subpixelStep, screenPxRange, edge, aaStrength));
}

vec4 composeSubpixelOutput(vec3 coverage, float grayscaleAlpha) {
  float edgeWeight = 1.0 - abs(grayscaleAlpha * 2.0 - 1.0);        // Convert alpha to edge weight (1.0 at edge, 0.0 at fully transparent/opaque)
  float blendStrength = clamp(sdf.subpixelBlendStrength, 0.0, 1.0);
  float subpixelBlend = clamp(edgeWeight * blendStrength, 0.0, blendStrength);       // Blend factor for subpixel coverage (max controlled at runtime)
  vec3 filteredCoverage = mix(                                     // Blend between grayscale alpha and subpixel coverage based on edge weight
    vec3(grayscaleAlpha), 
    coverage, 
    subpixelBlend
  );
  filteredCoverage = clamp(filteredCoverage, 0.0, 1.0);            // Ensure coverage is within valid range

  float outAlpha = fragColor.a * max(
    max(filteredCoverage.r, filteredCoverage.g), 
    filteredCoverage.b
  );
  vec3 outRgb = fragColor.rgb * filteredCoverage * fragColor.a;
  return vec4(outRgb, outAlpha);
}

void main() {
  vec2 texSize = vec2(textureSize(uiTexture, 0));
  vec2 unitRange = vec2(max(1.0, sdf.pxRange)) / max(texSize, vec2(1.0));
  vec2 screenTexelScale = vec2(1.0) / max(abs(dFdx(fragUv)) + abs(dFdy(fragUv)), vec2(1e-5));
  float screenPxRange = max(0.5 * dot(unitRange, screenTexelScale), 0.75);
  vec2 subpixelStep = dFdx(fragUv) / 3.0;
  float aaStrength = clamp(sdf.aaStrength, 0.1, 0.6);
  float edge = clamp(sdf.edge, 0.35, 0.65);

  if (sdf.msdfMode > 0.5) {
    float alpha = sampleMsdfAlpha(fragUv, screenPxRange, edge, aaStrength);
    vec3 coverage = sampleMsdfCoverageRgb(fragUv, subpixelStep, screenPxRange, edge, aaStrength);
    outColor = composeSubpixelOutput(coverage, alpha);
  }
  else
  {
    vec2 du = dFdx(fragUv) * 0.5;
    vec2 dv = dFdy(fragUv) * 0.5;

    float alphaCenter = sampleSdfAlpha(fragUv, screenPxRange, aaStrength, edge);
    float alphaSupersample = 0.25 * (
        sampleSdfAlpha(fragUv + du, screenPxRange, aaStrength, edge) +
        sampleSdfAlpha(fragUv - du, screenPxRange, aaStrength, edge) +
        sampleSdfAlpha(fragUv + dv, screenPxRange, aaStrength, edge) +
        sampleSdfAlpha(fragUv - dv, screenPxRange, aaStrength, edge));

    float supersampleBlend = mix(0.08, 0.22, (aaStrength - 0.1) / 0.5);
    float alpha = mix(alphaCenter, alphaSupersample, supersampleBlend);
    alpha = clamp((alpha - 0.5) * 1.12 + 0.5, 0.0, 1.0);
    vec3 coverage = sampleSdfCoverageRgb(fragUv, subpixelStep, screenPxRange, aaStrength, edge);
    outColor = composeSubpixelOutput(coverage, alpha);
  }
}
