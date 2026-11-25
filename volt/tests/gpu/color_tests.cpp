#include "../math/check.hpp"

#include <gpu/color.hpp>

#include <cmath>

using namespace volt;
using namespace volt::gpu;

namespace
{
    bool near(float a, float b, float tolerance = 1e-4f)
    {
        return std::abs(a - b) <= tolerance;
    }

    bool near(const math::vec3f &a, const math::vec3f &b, float tolerance = 1e-4f)
    {
        return near(a[0], b[0], tolerance) && near(a[1], b[1], tolerance) && near(a[2], b[2], tolerance);
    }

    void srgb_curve()
    {
        CHECK(near(srgb_to_linear(0.0f), 0.0f));
        CHECK(near(srgb_to_linear(1.0f), 1.0f));
        CHECK(near(srgb_to_linear(0.5f), 0.214041f));
        CHECK(near(linear_to_srgb(0.18f), 0.461356f)); // middle gray

        // The two pieces of the curve meet, and the curve inverts.
        CHECK(near(srgb_to_linear(0.04045f), 0.0031308f, 1e-6f));
        for (int i = 0; i <= 255; ++i)
        {
            const float encoded = static_cast<float>(i) / 255.0f;
            CHECK(near(linear_to_srgb(srgb_to_linear(encoded)), encoded, 1e-5f));
        }
    }

    // Reference points of SMPTE ST 2084.
    void pq_curve()
    {
        CHECK(near(nits_to_pq(0.0f), 0.0f, 1e-6f));
        CHECK(near(nits_to_pq(100.0f), 0.508078f));
        CHECK(near(nits_to_pq(1000.0f), 0.751827f));
        CHECK(near(nits_to_pq(10000.0f), 1.0f));
        CHECK(near(nits_to_pq(20000.0f), 1.0f)); // clamped
        CHECK(near(nits_to_pq(-5.0f), 0.0f, 1e-6f));
    }

    void gamut()
    {
        // White is white in both, and every sRGB color lies inside BT.2020.
        CHECK(near(rec709_to_rec2020({1.0f, 1.0f, 1.0f}), {1.0f, 1.0f, 1.0f}));
        CHECK(near(rec709_to_rec2020({1.0f, 0.0f, 0.0f}), {0.627404f, 0.069097f, 0.016391f}));
    }

    void encoding()
    {
        const math::vec3f gray{0.18f, 0.18f, 0.18f};
        const math::vec3f white{1.0f, 1.0f, 1.0f};
        const math::vec3f bright{4.0f, 4.0f, 4.0f};

        // SDR: the plain sRGB curve, and nothing above white.
        const output sdr{};
        CHECK(near(encode(gray, sdr), {0.461356f, 0.461356f, 0.461356f}));
        CHECK(near(encode(bright, sdr), white));
        CHECK(near(encode({-1.0f, 0.5f, 2.0f}, sdr), {0.0f, 0.735357f, 1.0f}));

        // What a style sheet says is what an SDR window gets.
        const math::vec3f authored{0.09f, 0.10f, 0.12f};
        CHECK(near(encode(srgb_to_linear(authored), sdr), authored, 1e-5f));

        // scRGB: linear, scaled so that white lands on the display's SDR white.
        const output scrgb{color_encoding::scrgb, 200.0f};
        CHECK(near(encode(white, scrgb), {2.5f, 2.5f, 2.5f}));
        CHECK(near(encode(bright, scrgb), {10.0f, 10.0f, 10.0f}));
        CHECK(near(encode(white, {color_encoding::scrgb, 80.0f}), white));

        // HDR10: white at 200 nits, four times that at 800, both through PQ.
        const output hdr10{color_encoding::hdr10, 200.0f};
        CHECK(near(encode(white, hdr10)[0], nits_to_pq(200.0f)));
        CHECK(near(encode(bright, hdr10)[1], nits_to_pq(800.0f)));
        CHECK(near(encode({0.0f, 0.0f, 0.0f}, hdr10), {0.0f, 0.0f, 0.0f}, 1e-6f));

        // A saturated sRGB red is not a saturated BT.2020 red.
        const math::vec3f red = encode({1.0f, 0.0f, 0.0f}, hdr10);
        CHECK(near(red[0], nits_to_pq(200.0f * 0.627404f)));
        CHECK(red[1] > 0.0f && red[2] > 0.0f && red[0] > red[1] && red[1] > red[2]);
    }
} // namespace

int main()
{
    srgb_curve();
    pq_curve();
    gamut();
    encoding();
    return volt::tests::result("gpu color");
}
