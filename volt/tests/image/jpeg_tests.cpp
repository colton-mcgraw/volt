#include "../math/check.hpp"
#include "files.hpp"

#include <image/jpeg.hpp>
#include <image/png.hpp>

#include <chrono>
#include <cmath>
#include <cstring>

using namespace volt;
namespace img = volt::image;
namespace jpeg = volt::image::jpeg;
namespace png = volt::image::png;
using img::pixel_format;

namespace
{
    struct difference
    {
        int largest = 0;
        double mean = 0.0;
        double psnr = 0.0;
    };

    // Between two images of the same size and channel count.
    difference compare(const img::image &a, const img::image &b)
    {
        difference result;
        if (a.width() != b.width() || a.height() != b.height() || a.format() != b.format())
        {
            result.largest = 999;
            return result;
        }
        double total = 0.0;
        double squares = 0.0;
        for (std::size_t i = 0; i < a.size_bytes(); ++i)
        {
            const int d = std::abs(int{a.data()[i]} - int{b.data()[i]});
            result.largest = std::max(result.largest, d);
            total += d;
            squares += static_cast<double>(d) * d;
        }
        const auto count = static_cast<double>(a.size_bytes());
        result.mean = total / count;
        result.psnr = squares == 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / (squares / count));
        return result;
    }

    img::image load_png(const char *name)
    {
        auto decoded = png::decode(tests::read_bytes(tests::asset(name)));
        if (!decoded)
        {
            std::fprintf(stderr, "%s: %s\n", name, decoded.error().c_str());
            return {};
        }
        return std::move(*decoded);
    }

    // Each test file against libjpeg-turbo's decoding of it (made with
    // Pillow when the files were): integer versus floating point IDCT, so a
    // level or two apart at most.
    void against_libjpeg()
    {
        const char *names[] = {"jpeg_444",         "jpeg_422",           "jpeg_420",
                               "jpeg_progressive_420", "jpeg_progressive_444", "jpeg_restart",
                               "jpeg_progressive_restart", "jpeg_gray",     "jpeg_gray_progressive",
                               "jpeg_cmyk",        "jpeg_exif6"};
        const img::image source = load_png("tests/jpeg_source.png");
        for (const char *name : names)
        {
            const std::string base = std::string("tests/") + name;
            auto decoded = jpeg::decode(tests::read_bytes(tests::asset((base + ".jpg").c_str())));
            CHECK(decoded.has_value());
            if (!decoded)
            {
                std::fprintf(stderr, "%s: %s\n", name, decoded.error().c_str());
                continue;
            }
            const img::image reference = load_png((base + ".ref.png").c_str());
            const difference d = compare(*decoded, reference);
            std::printf("%-26s %2u x %2u %-6s vs libjpeg: largest %d, mean %.3f", name, decoded->width(),
                        decoded->height(), img::name(decoded->format()), d.largest, d.mean);
            if (decoded->format() == pixel_format::rgb8 && decoded->width() == source.width())
                std::printf(", PSNR vs original %.1f dB", compare(*decoded, source).psnr);
            std::printf("\n");
            CHECK(d.largest <= 3);
            CHECK(d.mean < 0.5);
        }

        // The orientation can be left alone.
        jpeg::decode_options as_stored;
        as_stored.apply_orientation = false;
        auto stored = jpeg::decode(tests::read_bytes(tests::asset("tests/jpeg_exif6.jpg")), as_stored);
        CHECK(stored && stored->width() == 53 && stored->height() == 37);
    }

    void codec_render_test()
    {
        const auto bytes = tests::read_bytes(tests::asset("codec-render-test.jpg"));
        const auto start = std::chrono::steady_clock::now();
        auto decoded = jpeg::decode(bytes);
        const auto took = std::chrono::steady_clock::now() - start;
        CHECK(decoded.has_value());
        if (!decoded)
            return;
        std::printf("codec-render-test.jpg decoded in %.2f ms\n", std::chrono::duration<double, std::milli>(took).count());

        // The same picture as the PNG, within JPEG's loss.
        const img::image reference = img::convert(load_png("codec-render-test.png"), pixel_format::rgb8);
        const difference d = compare(*decoded, reference);
        std::printf("codec-render-test.jpg vs .png: PSNR %.1f dB, largest %d\n", d.psnr, d.largest);
        CHECK(d.psnr > 30.0);
        tests::write_bytes(tests::output("codec-render-test_from_jpeg.png"), *png::encode(*decoded));
    }

    void round_trips()
    {
        const img::image source = load_png("tests/jpeg_source.png");
        struct variant
        {
            jpeg::chroma_subsampling subsampling;
            const char *name;
        };
        const variant variants[] = {{jpeg::chroma_subsampling::none, "444"},
                                    {jpeg::chroma_subsampling::half, "420"},
                                    {jpeg::chroma_subsampling::horizontal, "422"},
                                    {jpeg::chroma_subsampling::vertical, "440"}};
        for (const variant &v : variants)
            for (const bool optimize : {false, true})
            {
                jpeg::encode_options options;
                options.quality = 92;
                options.subsampling = v.subsampling;
                options.optimize_huffman = optimize;
                auto encoded = jpeg::encode(source, options);
                CHECK(encoded.has_value());
                if (!encoded)
                    continue;
                auto decoded = jpeg::decode(*encoded);
                CHECK(decoded.has_value());
                if (!decoded)
                    continue;
                const difference d = compare(*decoded, source);
                std::printf("encoded %s, %s Huffman tables: %zu bytes, PSNR %.1f dB\n", v.name,
                            optimize ? "optimized" : "standard", encoded->size(), d.psnr);
                // The test picture has a one-pixel colored line and a hard
                // edged red square, which halved chroma cannot keep:
                // libjpeg's own 4:2:0 file of it is at 24 dB.
                CHECK(d.psnr > (v.subsampling == jpeg::chroma_subsampling::none ? 34.0 : 23.0));
                const std::string file = std::string("encoded_") + v.name + (optimize ? "_optimized" : "") + ".jpg";
                tests::write_bytes(tests::output(file.c_str()), *encoded);
            }

        // Quality trades size for fidelity.
        jpeg::encode_options low;
        low.quality = 30;
        jpeg::encode_options high;
        high.quality = 98;
        high.subsampling = jpeg::chroma_subsampling::none;
        const auto small = jpeg::encode(source, low);
        const auto large = jpeg::encode(source, high);
        CHECK(small->size() < large->size());
        CHECK(compare(*jpeg::decode(*small), source).psnr < compare(*jpeg::decode(*large), source).psnr);
        CHECK(compare(*jpeg::decode(*large), source).psnr > 38.0);

        // Gray stays gray; alpha is dropped.
        const img::image gray = img::convert(source, pixel_format::gray8);
        auto gray_decoded = jpeg::decode(*jpeg::encode(gray));
        CHECK(gray_decoded && gray_decoded->format() == pixel_format::gray8);
        CHECK(compare(*gray_decoded, gray).psnr > 35.0);
        auto from_rgba = jpeg::decode(*jpeg::encode(img::convert(source, pixel_format::rgba8)));
        CHECK(from_rgba && from_rgba->format() == pixel_format::rgb8);

        // A large picture, for timing.
        auto logo = png::decode(tests::read_bytes(tests::asset("volt.png")));
        CHECK(logo.has_value());
        if (logo)
        {
            const auto start = std::chrono::steady_clock::now();
            auto encoded = jpeg::encode(*logo);
            const auto middle = std::chrono::steady_clock::now();
            auto decoded = jpeg::decode(*encoded);
            const auto end = std::chrono::steady_clock::now();
            CHECK(decoded.has_value());
            if (decoded)
            {
                const difference d = compare(*decoded, *logo);
                std::printf("volt.png as JPEG (1080 x 1080, 4:2:0, q90): %zu bytes, encode %.1f ms, decode %.1f ms, "
                            "PSNR %.1f dB\n",
                            encoded->size(), std::chrono::duration<double, std::milli>(middle - start).count(),
                            std::chrono::duration<double, std::milli>(end - middle).count(), d.psnr);
                CHECK(d.psnr > 38.0);
                tests::write_bytes(tests::output("volt.jpg"), *encoded);
            }
        }
    }

    void unsupported()
    {
        auto bytes = tests::read_bytes(tests::asset("tests/jpeg_444.jpg"));
        std::size_t frame = 0;
        for (std::size_t i = 0; i + 1 < bytes.size(); ++i)
            if (bytes[i] == 0xFF && bytes[i + 1] == 0xC0)
            {
                frame = i;
                break;
            }
        CHECK(frame != 0);

        auto arithmetic = bytes;
        arithmetic[frame + 1] = 0xC9;
        auto result = jpeg::decode(arithmetic);
        CHECK(!result && result.error().find("arithmetic") != std::string::npos);

        auto twelve = bytes;
        twelve[frame + 4] = 12;
        result = jpeg::decode(twelve);
        CHECK(!result && result.error().find("12-bit") != std::string::npos);

        // A frame far larger than the file could fill is refused before the
        // coefficients are allocated.
        auto huge = bytes;
        huge[frame + 5] = 0x2B; // 11000 x 11000
        huge[frame + 6] = 0xF8;
        huge[frame + 7] = 0x2B;
        huge[frame + 8] = 0xF8;
        result = jpeg::decode(huge);
        CHECK(!result && result.error().find("far more") != std::string::npos);

        CHECK(!jpeg::decode({}).has_value());
        const std::uint8_t just_markers[] = {0xFF, 0xD8, 0xFF, 0xD9};
        CHECK(!jpeg::decode(just_markers).has_value());
    }

    void damaged_files()
    {
        tests::xorshift random;
        for (const char *name : {"codec-render-test.jpg", "tests/jpeg_progressive_restart.jpg", "tests/jpeg_cmyk.jpg"})
        {
            const auto good = tests::read_bytes(tests::asset(name));

            // Truncated anywhere: an error, or what arrived of the picture.
            int partial = 0;
            for (std::size_t length = 0; length < good.size(); ++length)
            {
                auto result = jpeg::decode(std::span<const std::uint8_t>(good).first(length));
                partial += result.has_value();
            }

            // Damaged anywhere: anything but a crash.
            int decoded = 0;
            for (int trial = 0; trial < 3000; ++trial)
            {
                std::vector<std::uint8_t> damaged = good;
                const int flips = 1 + static_cast<int>(random.next() % 8);
                for (int f = 0; f < flips; ++f)
                    damaged[random.next() % damaged.size()] ^= static_cast<std::uint8_t>(1u << (random.next() % 8));
                decoded += jpeg::decode(damaged).has_value();
            }
            std::printf("%s: %d of %zu truncations and %d of 3000 damaged copies still gave a picture\n", name,
                        partial, good.size(), decoded);
        }
    }
} // namespace

int main()
{
    against_libjpeg();
    codec_render_test();
    round_trips();
    unsupported();
    damaged_files();
    return volt::tests::result("image jpeg");
}
