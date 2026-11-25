#pragma once

#include <math/vector.hpp>

#include <algorithm>
#include <cmath>

// -------------------------------------------------------------------------
// color
// -------------------------------------------------------------------------
//
// Every pass renders in one working space, and exactly one step, the last
// one before the window, turns that into what the display expects. This file
// is that step on the CPU; assets/shaders/color.glsl is the same step for
// shaders, and the two have to stay in agreement.
//
// The working space is linear light with the sRGB (BT.709) primaries, where
// 1.0 is SDR white: the white of a sheet of paper, of the interface, of a
// schematic's background. Values above 1.0 are brighter than that, and only
// show as such on an HDR display. Colors written as sRGB (which is how every
// color in a style sheet is written) enter it through srgb_to_linear().
//
// Free of Vulkan on purpose, so it can be tested without a device.

namespace volt::gpu
{

    // How the values written to a window's image are interpreted.
    enum class color_encoding
    {
        // sRGB primaries and transfer curve, 0 to 1. Every SDR display.
        srgb,

        // scRGB: sRGB primaries, linear, in a floating point image. 1.0 is
        // 80 nits and there is no upper limit; negative values reach colors
        // outside the sRGB gamut.
        scrgb,

        // HDR10: BT.2020 primaries, PQ (SMPTE ST 2084) transfer curve, which
        // encodes absolute luminance up to 10000 nits into 0 to 1.
        hdr10,
    };

    // What the window currently wants. Changes while the application runs:
    // the user switches HDR on, or drags the window to another monitor.
    struct output
    {
        color_encoding encoding = color_encoding::srgb;

        // Luminance of SDR white in nits: how bright the working space's 1.0
        // comes out. Unused for color_encoding::srgb, where the display
        // decides.
        float white = 80.0f;

        friend bool operator==(const output &, const output &) = default;
    };

    // The luminance scRGB assigns to 1.0, in nits.
    inline constexpr float scrgb_white = 80.0f;

    // ---- transfer curves, one channel -----------------------------------

    inline float srgb_to_linear(float encoded) noexcept
    {
        return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
    }

    inline float linear_to_srgb(float linear) noexcept
    {
        return linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
    }

    // Absolute luminance in nits to a PQ signal in 0 to 1.
    inline float nits_to_pq(float nits) noexcept
    {
        constexpr float m1 = 0.1593017578125f;
        constexpr float m2 = 78.84375f;
        constexpr float c1 = 0.8359375f;
        constexpr float c2 = 18.8515625f;
        constexpr float c3 = 18.6875f;

        const float y = std::pow(std::clamp(nits / 10000.0f, 0.0f, 1.0f), m1);
        return std::pow((c1 + c2 * y) / (1.0f + c3 * y), m2);
    }

    // ---- colors ---------------------------------------------------------

    inline math::vec3f srgb_to_linear(const math::vec3f &encoded) noexcept
    {
        return {srgb_to_linear(encoded[0]), srgb_to_linear(encoded[1]), srgb_to_linear(encoded[2])};
    }

    // Linear BT.709 primaries to linear BT.2020 primaries. Same colors,
    // expressed in the wider gamut.
    inline math::vec3f rec709_to_rec2020(const math::vec3f &c) noexcept
    {
        return {
            0.627404f * c[0] + 0.329283f * c[1] + 0.043313f * c[2],
            0.069097f * c[0] + 0.919540f * c[1] + 0.011362f * c[2],
            0.016391f * c[0] + 0.088013f * c[1] + 0.895595f * c[2],
        };
    }

    // A working space color as the values to write to the window's image.
    inline math::vec3f encode(const math::vec3f &linear, const output &output) noexcept
    {
        switch (output.encoding)
        {
        case color_encoding::scrgb:
        {
            const float scale = output.white / scrgb_white;
            return {linear[0] * scale, linear[1] * scale, linear[2] * scale};
        }

        case color_encoding::hdr10:
        {
            const math::vec3f wide = rec709_to_rec2020(linear);
            return {nits_to_pq(wide[0] * output.white), nits_to_pq(wide[1] * output.white),
                    nits_to_pq(wide[2] * output.white)};
        }

        case color_encoding::srgb:
            break;
        }

        // Brighter than white does not exist here.
        const auto channel = [](float value) { return linear_to_srgb(std::clamp(value, 0.0f, 1.0f)); };
        return {channel(linear[0]), channel(linear[1]), channel(linear[2])};
    }

} // namespace volt::gpu
