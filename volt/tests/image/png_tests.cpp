#include "../math/check.hpp"
#include "files.hpp"
#include "png_vectors.hpp"

#include <image/compress.hpp>
#include <image/png.hpp>

#include <chrono>
#include <cstring>

using namespace volt;
namespace img = volt::image;
namespace png = volt::image::png;
using img::linear_encoding;
using img::load16;
using img::pixel_format;
using img::srgb_encoding;
using img::transfer;

namespace
{
    bool equal_pixels(const img::image &decoded, const std::uint8_t *expected, std::size_t count)
    {
        return decoded.size_bytes() == count && std::memcmp(decoded.data(), expected, count) == 0;
    }

    bool equal_pixels16(const img::image &decoded, const std::uint16_t *expected, std::size_t count)
    {
        if (decoded.size_bytes() != 2 * count)
            return false;
        for (std::size_t i = 0; i < count; ++i)
            if (load16(decoded.data() + 2 * i) != expected[i])
                return false;
        return true;
    }

    void hand_built_files()
    {
        using namespace png_vectors;

        auto palette = png::decode(palette_png);
        CHECK(palette.has_value());
        if (palette)
        {
            CHECK(palette->width() == 11 && palette->height() == 9);
            CHECK(palette->format() == pixel_format::rgba8);
            CHECK(equal_pixels(*palette, palette_expected, sizeof palette_expected));
        }

        auto rgba16 = png::decode(rgba16_png);
        CHECK(rgba16.has_value());
        if (rgba16)
        {
            CHECK(rgba16->format() == pixel_format::rgba16);
            CHECK(rgba16->encoding() == linear_encoding); // gAMA 1.0
            CHECK(equal_pixels16(*rgba16, rgba16_expected, std::size(rgba16_expected)));
        }

        auto gray1 = png::decode(gray1_png);
        CHECK(gray1.has_value());
        if (gray1)
        {
            CHECK(gray1->format() == pixel_format::gray8);
            CHECK(gray1->encoding() == srgb_encoding); // nothing said
            CHECK(equal_pixels(*gray1, gray1_expected, sizeof gray1_expected));
        }

        auto gray4 = png::decode(gray4_png);
        CHECK(gray4.has_value());
        if (gray4)
        {
            CHECK(gray4->format() == pixel_format::gray_alpha8);
            CHECK(equal_pixels(*gray4, gray4_expected, sizeof gray4_expected));
        }

        auto keyed = png::decode(rgb_key_png);
        CHECK(keyed.has_value());
        if (keyed)
        {
            CHECK(keyed->format() == pixel_format::rgba8);
            CHECK(keyed->encoding().curve == transfer::gamma);
            CHECK(std::abs(keyed->encoding().gamma - 1.8f) < 0.001f);
            CHECK(equal_pixels(*keyed, rgb_key_expected, sizeof rgb_key_expected));
        }

        auto tiny = png::decode(tiny_png);
        CHECK(tiny.has_value());
        if (tiny)
        {
            CHECK(tiny->format() == pixel_format::gray_alpha16);
            CHECK(load16(tiny->data()) == 1234 && load16(tiny->data() + 2) == 54321);
        }
    }

    img::image random_image(std::uint32_t width, std::uint32_t height, pixel_format format, tests::xorshift &random,
                              bool smooth)
    {
        img::image result = *img::image::create(width, height, format);
        for (std::size_t i = 0; i < result.size_bytes(); ++i)
            result.data()[i] = smooth ? static_cast<std::uint8_t>((i / 7 + (random.next() % 3)) & 0xFF)
                                      : static_cast<std::uint8_t>(random.next());
        return result;
    }

    void round_trips()
    {
        tests::xorshift random;
        const pixel_format formats[] = {pixel_format::gray8,  pixel_format::gray_alpha8,  pixel_format::rgb8,
                                        pixel_format::rgba8,  pixel_format::gray16,       pixel_format::gray_alpha16,
                                        pixel_format::rgb16,  pixel_format::rgba16};
        for (const pixel_format format : formats)
        {
            for (const int level : {0, 1, 6, 9})
            {
                for (const bool smooth : {false, true})
                {
                    img::image original = random_image(37, 23, format, random, smooth);
                    png::encode_options options;
                    options.compression = level;
                    auto encoded = png::encode(original, options);
                    CHECK(encoded.has_value());
                    if (!encoded)
                        continue;
                    auto decoded = png::decode(*encoded);
                    CHECK(decoded.has_value());
                    if (!decoded)
                        continue;
                    CHECK(decoded->format() == format);
                    CHECK(decoded->width() == 37 && decoded->height() == 23);
                    CHECK(decoded->encoding() == srgb_encoding);
                    CHECK(std::memcmp(decoded->data(), original.data(), original.size_bytes()) == 0);
                }
            }
        }

        // The encoding survives.
        img::image linear = random_image(4, 4, pixel_format::rgb16, random, false);
        linear.set_encoding(linear_encoding);
        CHECK(png::decode(*png::encode(linear))->encoding() == linear_encoding);
        linear.set_encoding({transfer::gamma, 1.8f});
        const auto gamma = png::decode(*png::encode(linear))->encoding();
        CHECK(gamma.curve == transfer::gamma && std::abs(gamma.gamma - 1.8f) < 0.001f);

        // Smooth images compress, and filtering helps.
        img::image gradient = *img::image::create(256, 256, pixel_format::rgb8);
        for (std::uint32_t y = 0; y < 256; ++y)
            for (std::uint32_t x = 0; x < 256; ++x)
            {
                std::uint8_t *p = gradient.at(x, y);
                p[0] = static_cast<std::uint8_t>(x);
                p[1] = static_cast<std::uint8_t>(y);
                p[2] = static_cast<std::uint8_t>((x + y) / 2);
            }
        png::encode_options unfiltered;
        unfiltered.filter = png::filter_strategy::none;
        const auto adaptive_size = png::encode(gradient)->size();
        const auto plain_size = png::encode(gradient, unfiltered)->size();
        CHECK(adaptive_size < plain_size);
        CHECK(adaptive_size < 256 * 256 * 3 / 20);
    }

    void test_assets()
    {
        const auto bytes = tests::read_bytes(tests::asset("codec-render-test.png"));
        CHECK(!bytes.empty());
        auto decoded = png::decode(bytes);
        CHECK(decoded.has_value());
        if (decoded)
        {
            CHECK(decoded->width() == 128 && decoded->height() == 128);
            CHECK(decoded->format() == pixel_format::rgba8);
        }

        const auto logo_bytes = tests::read_bytes(tests::asset("volt.png"));
        const auto start = std::chrono::steady_clock::now();
        auto logo = png::decode(logo_bytes);
        const auto took = std::chrono::steady_clock::now() - start;
        CHECK(logo.has_value());
        if (logo)
        {
            CHECK(logo->width() == 1080 && logo->height() == 1080);
            CHECK(logo->format() == pixel_format::rgb8);
            std::printf("volt.png (%zu bytes, 1080 x 1080) decoded in %.1f ms\n", logo_bytes.size(),
                        std::chrono::duration<double, std::milli>(took).count());

            // Re-encoded and decoded again, it is the same picture.
            const auto encode_start = std::chrono::steady_clock::now();
            auto again = png::encode(*logo);
            const auto encode_took = std::chrono::steady_clock::now() - encode_start;
            CHECK(again.has_value());
            if (again)
            {
                std::printf("volt.png re-encoded to %zu bytes in %.1f ms\n", again->size(),
                            std::chrono::duration<double, std::milli>(encode_took).count());
                tests::write_bytes(tests::output("volt_reencoded.png"), *again);
                auto back = png::decode(*again);
                CHECK(back.has_value() && std::memcmp(back->data(), logo->data(), logo->size_bytes()) == 0);
            }
        }
    }

    void damaged_files()
    {
        CHECK(!png::decode({}).has_value());
        const auto good = tests::read_bytes(tests::asset("codec-render-test.png"));

        // Every truncation fails cleanly, except for losing just the IEND
        // chunk: the picture is all there, and is read.
        for (std::size_t length = 0; length < good.size(); ++length)
        {
            auto result = png::decode(std::span<const std::uint8_t>(good).first(length));
            CHECK(!result.has_value() || length == good.size() - 12);
        }

        // Random damage never crashes. The CRCs catch nearly all of it, so
        // damage behind fixed-up CRCs too, to reach the decoder proper.
        tests::xorshift random;
        for (int trial = 0; trial < 4000; ++trial)
        {
            std::vector<std::uint8_t> damaged = good;
            const int flips = 1 + static_cast<int>(random.next() % 6);
            for (int f = 0; f < flips; ++f)
                damaged[8 + random.next() % (damaged.size() - 8)] ^= static_cast<std::uint8_t>(1u << (random.next() % 8));

            if (trial % 2 == 0)
            {
                // Recompute every chunk's CRC.
                std::size_t at = 8;
                while (at + 12 <= damaged.size())
                {
                    const std::size_t length = std::size_t{damaged[at]} << 24 | std::size_t{damaged[at + 1]} << 16 |
                                               std::size_t{damaged[at + 2]} << 8 | damaged[at + 3];
                    if (at + 12 + length > damaged.size())
                        break;
                    const std::uint32_t crc = compress::crc32(std::span<const std::uint8_t>(damaged).subspan(at + 4, length + 4));
                    for (int k = 0; k < 4; ++k)
                        damaged[at + 8 + length + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(crc >> (24 - 8 * k));
                    at += 12 + length;
                }
            }
            (void)png::decode(damaged);
        }

        // A header promising a huge image with no data behind it is refused
        // before anything is allocated for it.
        std::vector<std::uint8_t> huge(png_vectors::gray1_png, png_vectors::gray1_png + sizeof png_vectors::gray1_png);
        huge[16] = 0x00; // width 0x00FFFF..
        huge[17] = 0x00;
        huge[18] = 0xFF;
        huge[19] = 0xFF;
        huge[20] = 0x00;
        huge[21] = 0x00;
        huge[22] = 0x08;
        huge[23] = 0x00;
        const std::uint32_t crc = compress::crc32(std::span<const std::uint8_t>(huge).subspan(12, 17));
        for (int k = 0; k < 4; ++k)
            huge[29 + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(crc >> (24 - 8 * k));
        auto refused = png::decode(huge);
        CHECK(!refused.has_value());
        if (!refused)
            std::printf("a 65535 x 2048 header over 50 bytes of data: %s\n", refused.error().c_str());
    }
} // namespace

int main()
{
    hand_built_files();
    round_trips();
    test_assets();
    damaged_files();
    return volt::tests::result("image png");
}
