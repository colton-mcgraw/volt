#include "../math/check.hpp"
#include "files.hpp"

#include <image/bmp.hpp>
#include <image/compress.hpp>
#include <image/image.hpp>
#include <image/io.hpp>

#include <chrono>
#include <cmath>
#include <cstring>

using namespace volt;
namespace img = volt::image;
using img::pixel_format;

namespace
{
    bool near(float a, float b, float tolerance = 1e-4f)
    {
        return std::abs(a - b) <= tolerance;
    }

    img::image make(std::uint32_t width, std::uint32_t height, pixel_format format,
                    std::initializer_list<std::uint8_t> bytes)
    {
        img::image result = *img::image::create(width, height, format);
        std::copy(bytes.begin(), bytes.end(), result.data());
        return result;
    }

    // ---- the container ----------------------------------------------------------

    void container()
    {
        auto created = img::image::create(3, 2, pixel_format::rgb16);
        CHECK(created.has_value());
        if (created)
        {
            CHECK(created->stride() == 3 * 6);
            CHECK(created->size_bytes() == 36);
            CHECK(created->row(1).data() == created->data() + 18);
            CHECK(created->at(2, 1) == created->data() + 18 + 12);
            CHECK(std::all_of(created->pixels().begin(), created->pixels().end(), [](std::uint8_t b) { return b == 0; }));
            CHECK(created->encoding() == img::srgb_encoding && !created->premultiplied());
        }
        CHECK(!img::image::create(0, 5, pixel_format::rgba8).has_value());
        CHECK(!img::image::create(img::max_dimension + 1, 1, pixel_format::rgba8).has_value());
        auto huge = img::image::create(60000, 60000, pixel_format::gray8);
        CHECK(!huge && huge.error().find("60000") != std::string::npos);

        CHECK(img::pixel_size(pixel_format::gray_alpha16) == 4);
        CHECK(img::with_depth(pixel_format::rgb8, true) == pixel_format::rgb16);
        CHECK(img::make_format(2, false) == pixel_format::gray_alpha8);
        CHECK(img::has_alpha(pixel_format::rgba16) && !img::has_alpha(pixel_format::rgb8));
    }

    void conversions()
    {
        // 8 to 16 bits and back is exact for every value.
        img::image ramp = *img::image::create(256, 1, pixel_format::gray8);
        for (int i = 0; i < 256; ++i)
            ramp.data()[i] = static_cast<std::uint8_t>(i);
        const img::image wide = img::convert(ramp, pixel_format::gray16);
        CHECK(img::load16(wide.at(255, 0)) == 65535 && img::load16(wide.at(1, 0)) == 257);
        const img::image back = img::convert(wide, pixel_format::gray8);
        CHECK(std::memcmp(back.data(), ramp.data(), 256) == 0);

        // Channels: gray spreads, luma gathers, alpha appears opaque.
        const img::image rgb = make(2, 1, pixel_format::rgb8, {255, 0, 0, 10, 20, 30});
        const img::image rgba = img::convert(rgb, pixel_format::rgba8);
        CHECK(rgba.at(0, 0)[3] == 255 && rgba.at(1, 0)[2] == 30);
        const img::image gray = img::convert(rgb, pixel_format::gray8);
        CHECK(gray.at(0, 0)[0] == 54); // 0.2126 * 255
        const img::image spread = img::convert(make(1, 1, pixel_format::gray_alpha8, {77, 99}), pixel_format::rgba16);
        CHECK(img::load16(spread.data()) == 77 * 257 && img::load16(spread.data() + 4) == 77 * 257 &&
              img::load16(spread.data() + 6) == 99 * 257);

        // Transfer curves invert.
        const img::color_encoding gamma18{img::transfer::gamma, 1.8f};
        for (const img::color_encoding &e : {img::srgb_encoding, img::linear_encoding, gamma18})
            for (float v = 0.0f; v <= 1.0f; v += 0.05f)
                CHECK(near(img::from_linear(e, img::to_linear(e, v)), v, 1e-5f));
        CHECK(near(img::to_linear(img::srgb_encoding, 0.5f), 0.214041f));

        // Re-encoding: sRGB 128 is 21.6% of the light.
        img::image mid = make(1, 1, pixel_format::gray8, {128});
        const img::image linear = img::reencode(img::convert(mid, pixel_format::gray16), img::linear_encoding);
        CHECK(std::abs(static_cast<int>(img::load16(linear.data())) - static_cast<int>(0.2158f * 65535.0f)) < 40);
        CHECK(img::to_rgba8_srgb(linear).at(0, 0)[0] == 128);
        CHECK(img::reencode(linear, img::srgb_encoding).encoding() == img::srgb_encoding);
    }

    void alpha()
    {
        // Premultiplied in linear light: encode(decode(c) * a).
        img::image p = make(1, 1, pixel_format::rgba8, {255, 128, 0, 128});
        img::premultiply(p);
        CHECK(p.premultiplied());
        const float a = 128.0f / 255.0f;
        const auto expected = [&](std::uint8_t c) {
            const float linear = img::to_linear(img::srgb_encoding, c / 255.0f) * a;
            return static_cast<int>(std::lround(img::from_linear(img::srgb_encoding, linear) * 255.0f));
        };
        CHECK(p.at(0, 0)[0] == expected(255) && p.at(0, 0)[1] == expected(128) && p.at(0, 0)[2] == 0);
        CHECK(p.at(0, 0)[3] == 128);
        img::premultiply(p); // already: nothing happens
        CHECK(p.at(0, 0)[0] == expected(255));
        img::unpremultiply(p);
        CHECK(!p.premultiplied());
        CHECK(std::abs(int{p.at(0, 0)[0]} - 255) <= 1 && std::abs(int{p.at(0, 0)[1]} - 128) <= 1);

        // Saved, a premultiplied image comes back straight.
        img::premultiply(p);
        CHECK(!img::to_rgba8_srgb(p).premultiplied());
    }

    void scaling()
    {
        // A black and white checkerboard shrinks to the gray of half the
        // light, which sRGB writes as 188, not 128.
        img::image checker = *img::image::create(4, 4, pixel_format::gray8);
        for (std::uint32_t y = 0; y < 4; ++y)
            for (std::uint32_t x = 0; x < 4; ++x)
                checker.at(x, y)[0] = (x + y) % 2 == 0 ? 255 : 0;
        const img::image half = img::downsample(checker);
        CHECK(half.width() == 2 && half.height() == 2);
        CHECK(std::abs(int{half.at(1, 1)[0]} - 188) <= 1);

        // Transparent pixels do not bleed their color in.
        const img::image pair = make(2, 1, pixel_format::rgba8, {255, 0, 0, 255, 0, 255, 0, 0});
        const img::image one = img::downsample(pair);
        CHECK(one.at(0, 0)[0] == 255 && one.at(0, 0)[1] == 0 && std::abs(int{one.at(0, 0)[3]} - 128) <= 1);

        // Odd sizes and arbitrary factors keep the average.
        img::image gradient = *img::image::create(7, 5, pixel_format::gray16);
        gradient.set_encoding(img::linear_encoding);
        double sum = 0.0;
        for (std::uint32_t y = 0; y < 5; ++y)
            for (std::uint32_t x = 0; x < 7; ++x)
            {
                const auto v = static_cast<std::uint16_t>((x * 5 + y) * 1000);
                img::store16(gradient.at(x, y), v);
                sum += v;
            }
        const img::image small = img::resize(gradient, 3, 2);
        double small_sum = 0.0;
        for (std::uint32_t y = 0; y < 2; ++y)
            for (std::uint32_t x = 0; x < 3; ++x)
                small_sum += img::load16(small.at(x, y));
        CHECK(std::abs(small_sum / 6.0 - sum / 35.0) < 2.0);

        const auto chain = img::mip_chain(gradient);
        CHECK(chain.size() == 3 && chain[1].width() == 3 && chain[1].height() == 2 && chain[2].width() == 1 &&
              chain[2].height() == 1);
        CHECK(img::mip_chain(gradient, 2).size() == 2);
    }

    void orientation()
    {
        // 3 x 2, each pixel its own index.
        const img::image source = make(3, 2, pixel_format::gray8, {0, 1, 2, 3, 4, 5});
        const auto values = [](const img::image &i) {
            std::vector<int> out;
            for (std::size_t k = 0; k < i.size_bytes(); ++k)
                out.push_back(i.data()[k]);
            return out;
        };
        CHECK(values(img::apply_orientation(source, 1)) == std::vector<int>({0, 1, 2, 3, 4, 5}));
        CHECK(values(img::apply_orientation(source, 2)) == std::vector<int>({2, 1, 0, 5, 4, 3}));
        CHECK(values(img::apply_orientation(source, 3)) == std::vector<int>({5, 4, 3, 2, 1, 0}));
        CHECK(values(img::apply_orientation(source, 4)) == std::vector<int>({3, 4, 5, 0, 1, 2}));
        // The rest turn it on its side: 2 x 3.
        CHECK(values(img::apply_orientation(source, 5)) == std::vector<int>({0, 3, 1, 4, 2, 5}));
        CHECK(values(img::apply_orientation(source, 6)) == std::vector<int>({3, 0, 4, 1, 5, 2}));
        CHECK(values(img::apply_orientation(source, 7)) == std::vector<int>({5, 2, 4, 1, 3, 0}));
        CHECK(values(img::apply_orientation(source, 8)) == std::vector<int>({2, 5, 1, 4, 0, 3}));
        CHECK(img::apply_orientation(source, 6).width() == 2);

        img::image flipped = source;
        img::flip_vertical(flipped);
        CHECK(values(flipped) == std::vector<int>({3, 4, 5, 0, 1, 2}));
    }

    // ---- BMP ----------------------------------------------------------------------

    bool same_pixels(const img::image &a, const img::image &b)
    {
        return a.width() == b.width() && a.height() == b.height() && a.format() == b.format() &&
               std::memcmp(a.data(), b.data(), a.size_bytes()) == 0;
    }

    void bmp_files()
    {
        for (const char *name : {"bmp_1bit", "bmp_8bit", "bmp_24bit", "bmp_32bit_alpha"})
        {
            const std::string base = std::string("tests/") + name;
            auto decoded = img::bmp::decode(tests::read_bytes(tests::asset((base + ".bmp").c_str())));
            CHECK(decoded.has_value());
            if (!decoded)
            {
                std::fprintf(stderr, "%s: %s\n", name, decoded.error().c_str());
                continue;
            }
            const std::string reference_name = std::string(name) == "bmp_24bit" ? "tests/jpeg_source.png"
                                                                                 : base + ".ref.png";
            auto reference = img::load(tests::asset(reference_name.c_str()));
            CHECK(reference.has_value());
            if (reference)
                CHECK(same_pixels(*decoded, img::convert(*reference, decoded->format())));
        }

        // The repository's test picture: the same pixels as its PNG.
        auto bmp = img::load(tests::asset("codec-render-test.bmp"));
        auto png = img::load(tests::asset("codec-render-test.png"));
        CHECK(bmp && png);
        if (bmp && png)
            CHECK(same_pixels(img::convert(*bmp, pixel_format::rgba8), *png));

        // Hand-made: 2 x 2, top-down, 24-bit.
        std::vector<std::uint8_t> top_down = {'B', 'M', 0, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0, 40, 0, 0, 0,
                                              2,   0,   0, 0, 0xFE, 0xFF, 0xFF, 0xFF, 1, 0, 24, 0, 0, 0, 0, 0,
                                              0,   0,   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                              // row 0: blue, green; padded to 8
                                              255, 0, 0, 0, 255, 0, 0, 0,
                                              // row 1: red, white
                                              0, 0, 255, 255, 255, 255, 0, 0};
        auto small = img::bmp::decode(top_down);
        CHECK(small.has_value());
        if (small)
        {
            CHECK(small->format() == pixel_format::rgb8);
            CHECK(small->at(0, 0)[2] == 255 && small->at(1, 0)[1] == 255 && small->at(0, 1)[0] == 255);
        }

        // RLE8: a run of three of color 1, an absolute run of three (the
        // last past the edge) padded to 16 bits, end of bitmap.
        std::vector<std::uint8_t> rle = {'B', 'M', 0, 0, 0, 0, 0, 0, 0, 0, 62, 0, 0, 0, 40, 0, 0, 0, 5, 0, 0, 0,
                                         1,   0,   0, 0, 1, 0, 8, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                         2,   0,   0, 0, 0, 0, 0, 0,
                                         // palette: black, red
                                         0, 0, 0, 0, 0, 0, 255, 0,
                                         3, 1, 0, 3, 0, 1, 1, 0, 0, 1};
        auto runs = img::bmp::decode(rle);
        CHECK(runs.has_value());
        if (runs)
        {
            CHECK(runs->width() == 5 && runs->format() == pixel_format::rgba8);
            CHECK(runs->at(2, 0)[0] == 255 && runs->at(3, 0)[0] == 0 && runs->at(4, 0)[0] == 255);
        }

        // Damage never crashes.
        const auto good = tests::read_bytes(tests::asset("tests/bmp_8bit.bmp"));
        for (std::size_t length = 0; length < good.size(); length += 7)
            (void)img::bmp::decode(std::span<const std::uint8_t>(good).first(length));
        tests::xorshift random;
        for (int trial = 0; trial < 3000; ++trial)
        {
            std::vector<std::uint8_t> damaged = good;
            for (int f = 0; f < 4; ++f)
                damaged[random.next() % 80] ^= static_cast<std::uint8_t>(1u << (random.next() % 8));
            (void)img::bmp::decode(damaged);
        }
    }

    // ---- the front door ----------------------------------------------------------------

    void front_door()
    {
        CHECK(img::format_from_extension("a/b/Picture.JPG") == img::file_format::jpeg);
        CHECK(img::format_from_extension("x.svgz") == img::file_format::svg);
        CHECK(img::format_from_extension("x.tga") == img::file_format::unknown);

        struct expectation
        {
            const char *name;
            img::file_format format;
            std::uint32_t width;
        };
        for (const expectation &e : {expectation{"codec-render-test.png", img::file_format::png, 128},
                                     expectation{"codec-render-test.jpg", img::file_format::jpeg, 128},
                                     expectation{"codec-render-test.bmp", img::file_format::bmp, 128},
                                     expectation{"volt.svg", img::file_format::svg, 1080}})
        {
            const auto bytes = tests::read_bytes(tests::asset(e.name));
            CHECK(img::detect(bytes) == e.format);
            const auto start = std::chrono::steady_clock::now();
            auto loaded = img::load(tests::asset(e.name));
            const auto took = std::chrono::steady_clock::now() - start;
            CHECK(loaded && loaded->width() == e.width);
            std::printf("load(%s): %u x %u %s in %.2f ms\n", e.name, loaded ? loaded->width() : 0,
                        loaded ? loaded->height() : 0, loaded ? img::name(loaded->format()) : "-",
                        std::chrono::duration<double, std::milli>(took).count());
        }

        // An SVG at a requested size, and compressed.
        img::load_options sized;
        sized.svg.width = 64;
        auto icon = img::load(tests::asset("volt.svg"), sized);
        CHECK(icon && icon->width() == 64 && icon->height() == 64);
        const auto svgz = compress::gzip_compress(tests::read_bytes(tests::asset("volt.svg")));
        auto unpacked = img::load(svgz, sized);
        CHECK(unpacked && icon && same_pixels(*unpacked, *icon));

        // Errors say which file and what is wrong.
        auto missing = img::load(std::filesystem::path(VOLT_ASSETS_DIR) / "images" / "no-such-file.png");
        CHECK(!missing && missing.error().find("no-such-file.png") != std::string::npos);
        const std::uint8_t text[] = {'h', 'e', 'l', 'l', 'o'};
        auto unknown = img::load(text);
        CHECK(!unknown && unknown.error().find("not an image") != std::string::npos);
        CHECK(!img::load(std::span<const std::uint8_t>{}).has_value());

        // Saving goes by extension, and round trips.
        auto picture = img::load(tests::asset("codec-render-test.png"));
        CHECK(picture.has_value());
        if (!picture)
            return;
        const auto png_path = tests::output("saved.png");
        const auto jpg_path = tests::output("saved.jpeg");
        CHECK(img::save(png_path, *picture).has_value());
        CHECK(img::save(jpg_path, *picture).has_value());
        auto png_back = img::load(png_path);
        auto jpg_back = img::load(jpg_path);
        CHECK(png_back && same_pixels(*png_back, *picture));
        CHECK(jpg_back && jpg_back->format() == pixel_format::rgb8 && jpg_back->width() == 128);
        auto bmp_save = img::save(tests::output("saved.bmp"), *picture);
        CHECK(!bmp_save && bmp_save.error().find("does not write") != std::string::npos);
        CHECK(!img::save(tests::output("saved.xyz"), *picture).has_value());
        CHECK(!img::save(tests::output("saved.png"), img::image{}).has_value());
    }
} // namespace

int main()
{
    container();
    conversions();
    alpha();
    scaling();
    orientation();
    bmp_files();
    front_door();
    return volt::tests::result("image");
}
