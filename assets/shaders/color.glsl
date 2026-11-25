// The shader side of <gpu/color.hpp>: read the notes there first. The two
// have to stay in agreement.
//
// Shaders render in the working space (linear, sRGB primaries, 1.0 = SDR
// white). The pass that writes the window's image, and only that one, ends
// with encode_output().
//
//   #extension GL_GOOGLE_include_directive : require
//   #include "color.glsl"

#ifndef VOLT_COLOR_GLSL
#define VOLT_COLOR_GLSL

// volt::gpu::color_encoding
const int COLOR_ENCODING_SRGB = 0;
const int COLOR_ENCODING_SCRGB = 1;
const int COLOR_ENCODING_HDR10 = 2;

// The luminance scRGB assigns to 1.0, in nits.
const float SCRGB_WHITE = 80.0;

vec3 srgb_to_linear(vec3 encoded) {
  return mix(encoded / 12.92, pow((encoded + 0.055) / 1.055, vec3(2.4)), greaterThan(encoded, vec3(0.04045)));
}

vec3 linear_to_srgb(vec3 linear) {
  return mix(linear * 12.92, 1.055 * pow(linear, vec3(1.0 / 2.4)) - 0.055, greaterThan(linear, vec3(0.0031308)));
}

// Absolute luminance in nits to a PQ (SMPTE ST 2084) signal in 0 to 1.
vec3 nits_to_pq(vec3 nits) {
  const float m1 = 0.1593017578125;
  const float m2 = 78.84375;
  const float c1 = 0.8359375;
  const float c2 = 18.8515625;
  const float c3 = 18.6875;

  vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(m1));
  return pow((c1 + c2 * y) / (1.0 + c3 * y), vec3(m2));
}

// Linear BT.709 primaries to linear BT.2020 primaries.
vec3 rec709_to_rec2020(vec3 c) {
  // Column major: each line below is a column.
  const mat3 m = mat3(
    0.627404, 0.069097, 0.016391,
    0.329283, 0.919540, 0.088013,
    0.043313, 0.011362, 0.895595);
  return m * c;
}

// A working space color as the values to write to the window's image.
// `white` is the luminance of SDR white in nits (volt::gpu::output).
vec3 encode_output(vec3 linear, int encoding, float white) {
  if (encoding == COLOR_ENCODING_SCRGB)
    return linear * (white / SCRGB_WHITE);
  if (encoding == COLOR_ENCODING_HDR10)
    return nits_to_pq(rec709_to_rec2020(linear) * white);
  return linear_to_srgb(clamp(linear, 0.0, 1.0));
}

#endif
