#include <font/atlas.hpp>

#include <font/raster.hpp>

#include <algorithm>
#include <climits>
#include <cmath>

namespace volt::font
{

    namespace
    {
        pixel_rect unite(const pixel_rect &a, const pixel_rect &b) noexcept
        {
            if (a.empty())
                return b;
            if (b.empty())
                return a;
            const int left = std::min(a.x, b.x);
            const int top = std::min(a.y, b.y);
            const int right = std::max(a.x + a.width, b.x + b.width);
            const int bottom = std::max(a.y + a.height, b.y + b.height);
            return {left, top, right - left, bottom - top};
        }
    } // namespace

    std::size_t atlas::key_hash::operator()(const key &k) const noexcept
    {
        // splitmix64 over the packed key.
        std::uint64_t h = k.font * 0x9E3779B97F4A7C15ull;
        h ^= (std::uint64_t{k.size} << 32) | (std::uint64_t{k.glyph} << 16) | k.step;
        h ^= h >> 30;
        h *= 0xBF58476D1CE4E5B9ull;
        h ^= h >> 27;
        h *= 0x94D049BB133111EBull;
        h ^= h >> 31;
        return static_cast<std::size_t>(h);
    }

    atlas::atlas(const atlas_options &options) : m_options(options)
    {
        m_options.page_size = std::clamp(m_options.page_size, 16, 8192);
        m_options.padding = std::clamp(m_options.padding, 0, 16);
        m_options.subpixel_steps = std::clamp(m_options.subpixel_steps, 1, 64);
        m_options.max_pages = std::max<std::size_t>(m_options.max_pages, 1);
        if (!(m_options.distance_field_size > 0.0f))
            m_options.distance_field_size = 48.0f;
        if (!(m_options.distance_field_spread > 0.0f))
            m_options.distance_field_spread = 6.0f;
    }

    std::expected<atlas_glyph, std::string> atlas::glyph(const font &font, glyph_id glyph, float pixel_size,
                                                         float pen_x)
    {
        if (!font.valid())
            return std::unexpected("the font is empty");

        const bool fields = m_options.content == atlas_content::distance_field;
        key k;
        k.font = font.id();
        k.glyph = glyph;
        float shift = 0.0f;
        float size = m_options.distance_field_size;
        if (!fields)
        {
            if (!(pixel_size > 0.0f) || pixel_size > static_cast<float>(max_image_size))
                return atlas_glyph{};
            k.size = static_cast<std::uint32_t>(std::lround(pixel_size * 64.0f));
            size = static_cast<float>(k.size) / 64.0f;

            // The nearest step; the last one rounds up into the next pixel,
            // which is step 0 there (quad() moves the pen along with it).
            const int steps = m_options.subpixel_steps;
            const float fraction = std::isfinite(pen_x) ? pen_x - std::floor(pen_x) : 0.0f;
            const int step = static_cast<int>(std::lround(fraction * static_cast<float>(steps))) % steps;
            k.step = static_cast<std::uint16_t>(step);
            shift = static_cast<float>(step) / static_cast<float>(steps);
        }

        if (const auto found = m_glyphs.find(k); found != m_glyphs.end())
            return found->second;

        auto inserted = insert(font, glyph, size, shift);
        if (inserted)
            m_glyphs.emplace(k, *inserted);
        return inserted;
    }

    std::expected<atlas_glyph, std::string> atlas::insert(const font &font, glyph_id glyph, float pixel_size,
                                                          float shift)
    {
        const auto image = m_options.content == atlas_content::distance_field
                               ? distance_field(font, glyph, pixel_size, m_options.distance_field_spread)
                               : rasterize(font, glyph, pixel_size, shift);

        // A damaged glyph is remembered as blank rather than failed again
        // on every frame.
        atlas_glyph entry;
        if (!image || image->empty())
            return entry;

        const int padding = m_options.padding;
        const int width = image->width + padding;
        const int height = image->height + padding;
        if (width > m_options.page_size || height > m_options.page_size)
            return std::unexpected("the glyph is larger than an atlas page");

        int x = 0;
        int y = 0;
        std::size_t page = 0;
        for (; page < m_pages.size(); ++page)
        {
            if (place(m_pages[page], width, height, x, y))
                break;
        }
        if (page == m_pages.size())
        {
            if (m_pages.size() >= m_options.max_pages)
                return std::unexpected("the glyph atlas is full");
            const int size = m_options.page_size;
            page_data fresh;
            fresh.pixels.assign(static_cast<std::size_t>(size) * static_cast<std::size_t>(size), 0);
            fresh.skyline.push_back({0, 0, size});
            fresh.dirty = {0, 0, size, size};
            m_pages.push_back(std::move(fresh));
            if (!place(m_pages.back(), width, height, x, y))
                return std::unexpected("the glyph is larger than an atlas page");
        }

        // The padding is the block's top and left; its bottom and right are
        // the next block's padding, or the edge of the page.
        page_data &target = m_pages[page];
        const auto stride = static_cast<std::size_t>(m_options.page_size);
        const pixel_rect rect{x + padding, y + padding, image->width, image->height};
        for (int row = 0; row < rect.height; ++row)
        {
            const auto from = image->pixels.begin() + static_cast<std::ptrdiff_t>(row) * rect.width;
            std::copy(from, from + rect.width,
                      target.pixels.begin() +
                          static_cast<std::ptrdiff_t>(static_cast<std::size_t>(rect.y + row) * stride +
                                                      static_cast<std::size_t>(rect.x)));
        }
        target.dirty = unite(target.dirty, rect);

        const auto size = static_cast<float>(m_options.page_size);
        entry.page = static_cast<std::uint32_t>(page);
        entry.rect = rect;
        entry.uv_min = {static_cast<float>(rect.x) / size, static_cast<float>(rect.y) / size};
        entry.uv_max = {static_cast<float>(rect.x + rect.width) / size, static_cast<float>(rect.y + rect.height) / size};
        entry.offset = image->offset;
        return entry;
    }

    bool atlas::place(page_data &page, int width, int height, int &out_x, int &out_y)
    {
        // Skyline bottom-left: of all the spots along the skyline where the
        // block fits, the one where its bottom is highest, ties to the
        // narrowest ledge. The skyline is sorted by x and covers the page.
        const int size = m_options.page_size;
        std::vector<skyline_node> &nodes = page.skyline;
        std::size_t best = nodes.size();
        int best_bottom = INT_MAX;
        int best_width = INT_MAX;
        int best_y = 0;
        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
            if (nodes[i].x + width > size)
                break;
            int y = 0;
            int remaining = width;
            bool fits = true;
            for (std::size_t j = i; remaining > 0; ++j)
            {
                if (j >= nodes.size())
                {
                    fits = false;
                    break;
                }
                y = std::max(y, nodes[j].y);
                if (y + height > size)
                {
                    fits = false;
                    break;
                }
                remaining -= nodes[j].width;
            }
            if (!fits)
                continue;
            if (y + height < best_bottom || (y + height == best_bottom && nodes[i].width < best_width))
            {
                best = i;
                best_bottom = y + height;
                best_width = nodes[i].width;
                best_y = y;
            }
        }
        if (best == nodes.size())
            return false;

        const skyline_node node{nodes[best].x, best_y + height, width};
        nodes.insert(nodes.begin() + static_cast<std::ptrdiff_t>(best), node);

        // The new ledge covers what was under it.
        for (std::size_t i = best + 1; i < nodes.size();)
        {
            const int end = nodes[i - 1].x + nodes[i - 1].width;
            if (nodes[i].x >= end)
                break;
            const int shrink = end - nodes[i].x;
            nodes[i].x += shrink;
            nodes[i].width -= shrink;
            if (nodes[i].width > 0)
                break;
            nodes.erase(nodes.begin() + static_cast<std::ptrdiff_t>(i));
        }

        // Neighbouring ledges at the same height become one.
        for (std::size_t i = 0; i + 1 < nodes.size();)
        {
            if (nodes[i].y == nodes[i + 1].y)
            {
                nodes[i].width += nodes[i + 1].width;
                nodes.erase(nodes.begin() + static_cast<std::ptrdiff_t>(i) + 1);
            }
            else
            {
                ++i;
            }
        }

        out_x = node.x;
        out_y = best_y;
        return true;
    }

    std::expected<atlas_quad, std::string> atlas::quad(const font &font, glyph_id glyph, float pixel_size,
                                                       const math::vec2f &pen)
    {
        atlas_quad result;
        if (m_options.content == atlas_content::distance_field)
        {
            const auto entry = this->glyph(font, glyph, pixel_size, 0.0f);
            if (!entry)
                return std::unexpected(entry.error());
            if (entry->empty())
                return result;
            const float scale = pixel_size / m_options.distance_field_size;
            result.page = entry->page;
            result.position = {pen[0] + static_cast<float>(entry->offset[0]) * scale,
                               pen[1] + static_cast<float>(entry->offset[1]) * scale};
            result.size = {static_cast<float>(entry->rect.width) * scale,
                           static_cast<float>(entry->rect.height) * scale};
            result.uv_min = entry->uv_min;
            result.uv_max = entry->uv_max;
            return result;
        }

        // Split the pen into a whole pixel and a step within it, rounding to
        // the nearest step: the image holds the step, the quad the pixel.
        const auto steps = static_cast<float>(m_options.subpixel_steps);
        const float quantized = std::round(pen[0] * steps);
        const float whole = std::floor(quantized / steps);
        const float step = quantized - whole * steps;

        const auto entry = this->glyph(font, glyph, pixel_size, step / steps);
        if (!entry)
            return std::unexpected(entry.error());
        if (entry->empty())
            return result;
        result.page = entry->page;
        result.position = {whole + static_cast<float>(entry->offset[0]),
                           std::round(pen[1]) + static_cast<float>(entry->offset[1])};
        result.size = {static_cast<float>(entry->rect.width), static_cast<float>(entry->rect.height)};
        result.uv_min = entry->uv_min;
        result.uv_max = entry->uv_max;
        return result;
    }

    atlas_page atlas::page(std::size_t index) const noexcept
    {
        if (index >= m_pages.size())
            return {};
        return {m_options.page_size, m_options.page_size, m_pages[index].pixels};
    }

    pixel_rect atlas::take_dirty(std::size_t index) noexcept
    {
        if (index >= m_pages.size())
            return {};
        const pixel_rect dirty = m_pages[index].dirty;
        m_pages[index].dirty = {};
        return dirty;
    }

    void atlas::clear()
    {
        m_pages.clear();
        m_glyphs.clear();
        ++m_generation;
    }

} // namespace volt::font
