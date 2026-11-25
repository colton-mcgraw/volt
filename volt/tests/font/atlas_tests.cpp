#include "common.hpp"

#include <font/atlas.hpp>
#include <font/raster.hpp>

#include <cmath>

using namespace volt::font;
using volt::math::vec2f;
using volt::tests::font::arial;
using volt::tests::font::near;
using volt::tests::font::saira;

namespace
{
    struct placed
    {
        atlas_glyph glyph;
        const char *what;
    };

    bool contains(const pixel_rect &outer, const pixel_rect &inner)
    {
        return inner.x >= outer.x && inner.y >= outer.y && inner.x + inner.width <= outer.x + outer.width &&
               inner.y + inner.height <= outer.y + outer.height;
    }

    // No two glyphs of a page closer than the padding; all inside the page.
    void check_packing(const atlas &atlas, const std::vector<atlas_glyph> &glyphs)
    {
        const int size = atlas.options().page_size;
        const int padding = atlas.options().padding;
        int overlaps = 0;
        for (std::size_t i = 0; i < glyphs.size(); ++i)
        {
            const pixel_rect &a = glyphs[i].rect;
            if (a.empty())
                continue;
            CHECK(glyphs[i].page < atlas.page_count());
            CHECK(contains({0, 0, size, size}, a));
            CHECK(a.x >= padding && a.y >= padding);
            for (std::size_t j = i + 1; j < glyphs.size(); ++j)
            {
                const pixel_rect &b = glyphs[j].rect;
                if (b.empty() || glyphs[i].page != glyphs[j].page)
                    continue;
                // Grow one by the padding: they must still not touch.
                const bool apart = a.x + a.width + padding <= b.x || b.x + b.width + padding <= a.x ||
                                   a.y + a.height + padding <= b.y || b.y + b.height + padding <= a.y;
                if (!apart)
                    ++overlaps;
            }
        }
        CHECK(overlaps == 0);
    }

    void basics()
    {
        atlas glyphs;
        CHECK(glyphs.page_count() == 0 && glyphs.glyph_count() == 0);

        const glyph_id g = arial().glyph(U'g');
        const auto first = glyphs.glyph(arial(), g, 32.0f);
        CHECK(first.has_value() && !first->empty());
        if (!first)
            return;
        CHECK(glyphs.page_count() == 1 && glyphs.glyph_count() == 1);

        // The same request is a lookup.
        const auto again = glyphs.glyph(arial(), g, 32.0f, 7.0f); // whole pixels: same step
        CHECK(again && again->rect == first->rect && glyphs.glyph_count() == 1);

        // Its pixels are the rasterizer's.
        const auto image = rasterize(arial(), g, 32.0f);
        CHECK(image && image->width == first->rect.width && image->height == first->rect.height);
        CHECK(image && image->offset == first->offset);
        const atlas_page page = glyphs.page(first->page);
        CHECK(page.width == 1024 && page.height == 1024 && page.pixels.size() == 1024u * 1024u);
        bool same = true;
        for (int y = 0; image && y < image->height; ++y)
        {
            for (int x = 0; x < image->width; ++x)
            {
                const std::size_t at = static_cast<std::size_t>(first->rect.y + y) * 1024u +
                                       static_cast<std::size_t>(first->rect.x + x);
                same = same && page.pixels[at] == image->at(x, y);
            }
        }
        CHECK(same);

        // Texture coordinates are the rect over the page size.
        CHECK(near(first->uv_min[0], static_cast<float>(first->rect.x) / 1024.0f, 1e-7f));
        CHECK(near(first->uv_max[1], static_cast<float>(first->rect.y + first->rect.height) / 1024.0f, 1e-7f));

        // Other sizes, fonts and subpixel steps are other entries.
        CHECK(glyphs.glyph(arial(), g, 33.0f) && glyphs.glyph_count() == 2);
        CHECK(glyphs.glyph(saira(), saira().glyph(U'g'), 32.0f) && glyphs.glyph_count() == 3);
        const auto quarter = glyphs.glyph(arial(), g, 32.0f, 0.26f);
        CHECK(quarter && !(quarter->rect == first->rect) && glyphs.glyph_count() == 4);
        // 0.9 rounds to the next pixel's step 0, which is the first entry.
        const auto rounded = glyphs.glyph(arial(), g, 32.0f, 0.9f);
        CHECK(rounded && rounded->rect == first->rect && glyphs.glyph_count() == 4);

        // Blank glyphs take no room.
        const auto space = glyphs.glyph(arial(), arial().glyph(U' '), 32.0f);
        CHECK(space && space->empty());
        CHECK(!glyphs.glyph(font{}, 1, 32.0f));
    }

    void dirty_regions()
    {
        atlas glyphs({.page_size = 256});
        const auto a = glyphs.glyph(arial(), arial().glyph(U'A'), 20.0f);
        CHECK(a.has_value());
        // A new page is dirty everywhere, once.
        CHECK(glyphs.take_dirty(0) == (pixel_rect{0, 0, 256, 256}));
        CHECK(glyphs.take_dirty(0).empty());

        // Then exactly what was added.
        const auto b = glyphs.glyph(arial(), arial().glyph(U'B'), 20.0f);
        CHECK(b && glyphs.take_dirty(0) == b->rect);
        const auto c = glyphs.glyph(arial(), arial().glyph(U'C'), 20.0f);
        const auto d = glyphs.glyph(arial(), arial().glyph(U'D'), 20.0f);
        CHECK(c && d);
        if (c && d)
        {
            const pixel_rect dirty = glyphs.take_dirty(0);
            CHECK(contains(dirty, c->rect) && contains(dirty, d->rect));
            // The union and nothing more.
            CHECK(dirty.x == std::min(c->rect.x, d->rect.x));
            CHECK(dirty.y + dirty.height == std::max(c->rect.y + c->rect.height, d->rect.y + d->rect.height));
        }
        // Looking up does not dirty anything.
        CHECK(glyphs.glyph(arial(), arial().glyph(U'A'), 20.0f) && glyphs.take_dirty(0).empty());
        CHECK(glyphs.take_dirty(5).empty());
    }

    void packing()
    {
        // Printable ASCII at three sizes and four steps, both fonts.
        atlas glyphs;
        std::vector<atlas_glyph> all;
        int area = 0;
        for (const font &f : {arial(), saira()})
        {
            for (const float size : {12.0f, 16.0f, 32.0f})
            {
                for (char32_t c = 0x21; c < 0x7F; ++c)
                {
                    for (const float pen : {0.0f, 0.25f, 0.5f, 0.75f})
                    {
                        const auto entry = glyphs.glyph(f, f.glyph(c), size, pen);
                        CHECK(entry.has_value());
                        if (!entry)
                            continue;
                        all.push_back(*entry);
                        area += entry->rect.width * entry->rect.height;
                    }
                }
            }
        }
        check_packing(glyphs, all);
        CHECK(glyphs.page_count() == 1);
        std::printf("  %zu glyphs in %zu page(s), %.0f%% of a 1024 page covered by images\n", glyphs.glyph_count(),
                    glyphs.page_count(), 100.0 * area / (1024.0 * 1024.0));
    }

    void pages_and_limits()
    {
        // Small pages fill up and more are started, up to the limit.
        atlas glyphs({.page_size = 64, .max_pages = 3});
        std::vector<atlas_glyph> all;
        bool full = false;
        for (char32_t c = U'A'; c <= U'Z' && !full; ++c)
        {
            const auto entry = glyphs.glyph(arial(), arial().glyph(c), 40.0f);
            if (!entry)
            {
                full = true;
                CHECK(entry.error() == "the glyph atlas is full");
                break;
            }
            all.push_back(*entry);
        }
        CHECK(full);
        CHECK(glyphs.page_count() == 3);
        check_packing(glyphs, all);
        for (std::size_t page = 0; page < 3; ++page)
            CHECK(!glyphs.take_dirty(page).empty());

        // Too big for any page.
        CHECK(!glyphs.glyph(arial(), arial().glyph(U'W'), 200.0f));

        // Clearing starts over.
        const std::uint64_t generation = glyphs.generation();
        glyphs.clear();
        CHECK(glyphs.generation() != generation);
        CHECK(glyphs.page_count() == 0 && glyphs.glyph_count() == 0);
        CHECK(glyphs.glyph(arial(), arial().glyph(U'A'), 24.0f).has_value());
        CHECK(glyphs.page(7).pixels.empty());
    }

    void quads()
    {
        atlas glyphs;
        const glyph_id a = arial().glyph(U'A');

        // Pen at x 10.3: the nearest quarter is 10.25, so whole pixel 10 and
        // step 1; y rounds to 21.
        const auto quad = glyphs.quad(arial(), a, 16.0f, {10.3f, 20.6f});
        const auto entry = glyphs.glyph(arial(), a, 16.0f, 0.25f);
        CHECK(quad && entry);
        if (quad && entry)
        {
            CHECK(near(quad->position[0], 10.0f + static_cast<float>(entry->offset[0]), 1e-6f));
            CHECK(near(quad->position[1], 21.0f + static_cast<float>(entry->offset[1]), 1e-6f));
            CHECK(near(quad->size[0], static_cast<float>(entry->rect.width), 1e-6f));
            CHECK(quad->uv_min == entry->uv_min && quad->uv_max == entry->uv_max);
        }
        // 10.9 is 11 and step 0.
        const auto next = glyphs.quad(arial(), a, 16.0f, {10.9f, 0.0f});
        const auto zero = glyphs.glyph(arial(), a, 16.0f, 0.0f);
        CHECK(next && zero && near(next->position[0], 11.0f + static_cast<float>(zero->offset[0]), 1e-6f));
        CHECK(glyphs.quad(arial(), arial().glyph(U' '), 16.0f, {0.0f, 0.0f})->empty());

        // Distance fields: made once at the generation size, scaled to any.
        atlas fields({.content = atlas_content::distance_field, .distance_field_size = 32.0f,
                      .distance_field_spread = 4.0f});
        const auto small = fields.quad(arial(), a, 16.0f, {100.0f, 50.0f});
        const auto large = fields.quad(arial(), a, 128.0f, {100.0f, 50.0f});
        CHECK(small && large && fields.glyph_count() == 1);
        if (small && large)
        {
            CHECK(near(large->size[0], small->size[0] * 8.0f, 1e-3f));
            CHECK(near(large->position[0] - 100.0f, (small->position[0] - 100.0f) * 8.0f, 1e-3f));
            CHECK(large->uv_min == small->uv_min);
        }
        const auto field = fields.glyph(arial(), a, 999.0f);
        const auto direct = distance_field(arial(), a, 32.0f, 4.0f);
        CHECK(field && direct && field->rect.width == direct->width && field->offset == direct->offset);
    }
} // namespace

int main()
{
    basics();
    dirty_regions();
    packing();
    pages_and_limits();
    quads();
    return volt::tests::result("font atlas");
}
