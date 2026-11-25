#include "common.hpp"
#include "exercise.hpp"

#include <algorithm>
#include <cstring>
#include <random>

// Damaged fonts must load with an error or with some glyphs failing, never
// crash or hang. Each damaged copy is exercised in full (exercise.hpp); the
// test passing means every one of them came back.

using namespace volt::font;
using volt::tests::font::asset;
using volt::tests::font::exercise;
using volt::tests::font::read_file;

namespace
{
    using bytes = std::vector<std::byte>;

    std::uint32_t get16(const bytes &b, std::size_t at)
    {
        return (std::to_integer<std::uint32_t>(b[at]) << 8) | std::to_integer<std::uint32_t>(b[at + 1]);
    }

    std::uint32_t get32(const bytes &b, std::size_t at)
    {
        return (get16(b, at) << 16) | get16(b, at + 2);
    }

    void put16(bytes &b, std::size_t at, std::uint32_t value)
    {
        b[at] = static_cast<std::byte>((value >> 8) & 0xFFu);
        b[at + 1] = static_cast<std::byte>(value & 0xFFu);
    }

    void put32(bytes &b, std::size_t at, std::uint32_t value)
    {
        put16(b, at, value >> 16);
        put16(b, at + 2, value & 0xFFFFu);
    }

    struct table
    {
        std::uint32_t tag = 0;
        std::size_t offset = 0;
        std::size_t length = 0;
    };

    std::vector<table> tables(const bytes &file)
    {
        std::vector<table> result;
        const std::uint32_t count = get16(file, 4);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const std::size_t record = 12 + i * 16;
            result.push_back({get32(file, record), get32(file, record + 8), get32(file, record + 12)});
        }
        return result;
    }

    table find(const bytes &file, const char *name)
    {
        std::uint32_t tag = 0;
        for (int i = 0; i < 4; ++i)
            tag = (tag << 8) | static_cast<unsigned char>(name[i]);
        for (const table &t : tables(file))
        {
            if (t.tag == tag)
                return t;
        }
        return {};
    }

    // Where glyph `glyph`'s data starts in the file (long loca only).
    std::size_t glyph_offset(const bytes &file, std::uint32_t glyph)
    {
        return find(file, "glyf").offset + get32(file, find(file, "loca").offset + glyph * 4);
    }

    std::size_t copies = 0;

    // Loads and, if it loads, exercises. Returns whether it loaded.
    bool attempt(const bytes &damaged, std::uint32_t raster_every = 0)
    {
        ++copies;
        const auto loaded = font::load(damaged);
        if (!loaded)
            return false;
        (void)exercise(*loaded, raster_every);
        return true;
    }

    // Cut at every table's start and end and at a spread of lengths.
    void truncation(const bytes &original)
    {
        std::vector<std::size_t> lengths{0, 1, 4, 11, 12, 13, 27, 100};
        for (const table &t : tables(original))
        {
            lengths.push_back(t.offset);
            lengths.push_back(t.offset + 1);
            lengths.push_back(t.offset + t.length / 2);
            lengths.push_back(t.offset + t.length - 1);
        }
        for (int i = 1; i < 16; ++i)
            lengths.push_back(original.size() * static_cast<std::size_t>(i) / 16);

        int loaded = 0;
        for (const std::size_t length : lengths)
        {
            if (length > original.size())
                continue;
            const bytes damaged(original.begin(), original.begin() + static_cast<std::ptrdiff_t>(length));
            loaded += attempt(damaged) ? 1 : 0;
        }
        // Too short for the required tables is an error, not a font.
        CHECK(!font::load(bytes(original.begin(), original.begin() + 11)));
        std::printf("  truncated: %zu lengths, %d loaded anyway\n", lengths.size(), loaded);
    }

    // Random bit flips: in the table directory and table headers, where a
    // flip changes structure, and anywhere.
    void bit_flips(const bytes &original, std::uint32_t seed, int rounds)
    {
        std::mt19937 random(seed);
        const std::vector<table> all = tables(original);
        int loaded = 0;
        for (int round = 0; round < rounds; ++round)
        {
            bytes damaged = original;
            const int flips = 1 + round % 24;
            for (int flip = 0; flip < flips; ++flip)
            {
                std::size_t at = 0;
                switch (random() % 3)
                {
                case 0: // the directory
                    at = random() % (12 + all.size() * 16);
                    break;
                case 1: // the first 64 bytes of some table
                {
                    const table &t = all[random() % all.size()];
                    at = t.offset + random() % std::max<std::size_t>(std::min<std::size_t>(t.length, 64), 1);
                    break;
                }
                default:
                    at = random() % damaged.size();
                    break;
                }
                if (at < damaged.size())
                    damaged[at] ^= static_cast<std::byte>(1u << (random() % 8));
            }
            loaded += attempt(damaged, round % 8 == 0 ? 53 : 0) ? 1 : 0;
        }
        std::printf("  bit flips: %d rounds, %d loaded anyway\n", rounds, loaded);
    }

    // Damage aimed at the defences.
    void targeted(const bytes &arial)
    {
        // é (112) is e + acute. Make its first component itself: a cycle.
        {
            bytes damaged = arial;
            put16(damaged, glyph_offset(damaged, 112) + 12, 112);
            const auto f = font::load(damaged);
            CHECK(f.has_value());
            if (f)
            {
                CHECK(!f->glyph_outline(112));
                CHECK(f->glyph_outline(72).has_value()); // the rest is fine
                (void)exercise(*f, 0);
            }
        }
        // Å (99) is A + ring; é and Å referencing each other.
        {
            bytes damaged = arial;
            put16(damaged, glyph_offset(damaged, 112) + 12, 99);
            put16(damaged, glyph_offset(damaged, 99) + 12, 112);
            const auto f = font::load(damaged);
            CHECK(f && !f->glyph_outline(112) && !f->glyph_outline(99));
        }
        // A component that does not exist, and a point anchor past the end.
        {
            bytes damaged = arial;
            put16(damaged, glyph_offset(damaged, 112) + 12, 60000);
            const auto f = font::load(damaged);
            CHECK(f && !f->glyph_outline(112));

            bytes anchored = arial;
            const std::size_t component = glyph_offset(anchored, 112) + 10;
            // Clear ARGS_ARE_XY_VALUES: the arguments become point numbers.
            put16(anchored, component, get16(anchored, component) & ~0x0002u);
            const auto g = font::load(anchored);
            CHECK(g.has_value());
            if (g)
                (void)g->glyph_outline(112); // anchors (0, 0): valid or not, it returns
        }
        // A simple glyph claiming a huge number of contours.
        {
            bytes damaged = arial;
            put16(damaged, glyph_offset(damaged, 36), 0x7FFF);
            const auto f = font::load(damaged);
            CHECK(f && !f->glyph_outline(36));
        }
        // Counts that promise far more than the file holds.
        {
            bytes damaged = arial;
            put16(damaged, find(damaged, "maxp").offset + 4, 0xFFFF); // numGlyphs
            put16(damaged, find(damaged, "hhea").offset + 34, 0xFFFF); // numberOfHMetrics
            const auto f = font::load(damaged);
            CHECK(f.has_value());
            if (f)
            {
                CHECK(f->glyph_count() == 65535);
                CHECK(!f->glyph_outline(65000));
                (void)f->metrics(65000);
                (void)exercise(*f, 0);
            }
        }
        {
            // Far more cmap records than there are: the extra "records" are
            // whatever bytes follow. Anything may map, nothing may break.
            bytes damaged = arial;
            const table cmap = find(damaged, "cmap");
            put16(damaged, cmap.offset + 2, 0xFFFF);
            CHECK(attempt(damaged));

            // The three real records pointing past the end: nothing maps.
            bytes lost = arial;
            for (std::size_t i = 0; i < 3; ++i)
                put32(lost, cmap.offset + 4 + i * 8 + 4, 0x7FFFFFF0u);
            const auto f = font::load(lost);
            CHECK(f && f->glyph(U'A') == 0 && f->glyph(U'€') == 0);
        }
        {
            bytes damaged = arial;
            put16(damaged, find(damaged, "head").offset + 18, 0); // unitsPerEm
            CHECK(!font::load(damaged));
            bytes no_tables = arial;
            put16(no_tables, 4, 0);
            CHECK(!font::load(no_tables));
        }
        // Collections claiming faces beyond the file.
        {
            bytes collection(16, std::byte{0});
            put32(collection, 0, 0x74746366u);
            put32(collection, 8, 0xFFFFFFFFu);
            put32(collection, 12, 0xFFFFFFF0u);
            CHECK(!font::load(collection, 0));
            const auto count = font::face_count(collection);
            CHECK(count && *count == 1); // only one offset fits
            CHECK(!font::load_collection(collection));
        }
    }
} // namespace

int main()
{
    const bytes arial = read_file(asset("DefaultFont.ttf"));
    const bytes saira = read_file(asset("SairaStencil.ttf"));
    CHECK(!arial.empty() && !saira.empty());

    std::printf("DefaultFont.ttf\n");
    truncation(arial);
    bit_flips(arial, 1, 80);
    std::printf("SairaStencil.ttf\n");
    truncation(saira);
    bit_flips(saira, 2, 120);
    targeted(arial);

    std::printf("  %zu damaged copies, all returned\n", copies);
    return volt::tests::result("font corruption");
}
