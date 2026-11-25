#pragma once

#include <font/font.hpp>
#include <math/vector.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

// -------------------------------------------------------------------------
// atlas
// -------------------------------------------------------------------------
//
// Glyph images packed into pages, on demand: the first time a glyph is asked
// for at a size it is rasterized and placed, after that it is looked up. A
// renderer uploads the pages as single-channel textures (8-bit UNORM, linear:
// the values are coverage or distance, not colour) and draws one textured
// quad per glyph. Nothing here knows about the GPU; a page is bytes, and
// each page tracks the rectangle that changed since the renderer last took
// it, so uploads can be partial.
//
// Glyphs are keyed by font (font::id()), glyph, pixel size (to 1/64 px) and
// horizontal subpixel position: text is rasterized at `subpixel_steps`
// positions per pixel, so that its spacing is not rounded to whole pixels,
// at the price of up to that many images per glyph. Vertically, pen
// positions are rounded to whole pixels.
//
// Packing is skyline bottom-left, a pixel of padding between glyphs so that
// bilinear sampling never reads a neighbour. When a page is full a new one
// is started, up to max_pages; past that, glyph() fails until clear().
//
// Not thread safe: one atlas belongs to one thread (the renderer's).

namespace volt::font
{

    enum class atlas_content : std::uint8_t
    {
        // Antialiased coverage at the size asked for (raster.hpp).
        coverage,

        // Signed distance fields, generated once per glyph at
        // distance_field_size and scaled to any size when drawn.
        distance_field,
    };

    struct atlas_options
    {
        int page_size = 1024; // square pages, pixels
        int padding = 1;
        int subpixel_steps = 4; // 1 for whole pixels only
        std::size_t max_pages = 16;

        atlas_content content = atlas_content::coverage;
        float distance_field_size = 48.0f;  // px per em the fields are made at
        float distance_field_spread = 6.0f; // px, at that size
    };

    struct pixel_rect
    {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;

        bool empty() const noexcept { return width <= 0 || height <= 0; }
        friend bool operator==(const pixel_rect &, const pixel_rect &) = default;
    };

    struct atlas_glyph
    {
        std::uint32_t page = 0;
        pixel_rect rect; // in the page, padding not included

        // The rect in texture coordinates of its page, 0 to 1.
        math::vec2f uv_min{};
        math::vec2f uv_max{};

        // As glyph_image::offset: where the image's top-left corner goes
        // relative to the pen, in pixels of the size it was made at.
        math::vec2i offset{};

        // A blank glyph (a space), or one whose outline is damaged: nothing
        // to draw, but it still advances.
        bool empty() const noexcept { return rect.empty(); }
    };

    // Where to draw a glyph: the quad and what to sample for it.
    struct atlas_quad
    {
        std::uint32_t page = 0;
        math::vec2f position{}; // top-left, pixels, y down
        math::vec2f size{};
        math::vec2f uv_min{};
        math::vec2f uv_max{};

        bool empty() const noexcept { return size[0] <= 0.0f || size[1] <= 0.0f; }
    };

    struct atlas_page
    {
        int width = 0;
        int height = 0;
        std::span<const std::uint8_t> pixels; // rows top to bottom, width bytes each
    };

    // ---------------------------------------------------------------------
    // atlas
    // ---------------------------------------------------------------------
    class atlas
    {
    public:
        explicit atlas(const atlas_options &options = {});

        const atlas_options &options() const noexcept { return m_options; }

        // The glyph at `pixel_size` px per em, drawn with the pen at x
        // `pen_x` (only the fraction matters; coverage atlases only). Fails
        // only when the atlas is full or the glyph is too big for a page.
        std::expected<atlas_glyph, std::string> glyph(const font &font, glyph_id glyph, float pixel_size,
                                                      float pen_x = 0.0f);

        // The quad that draws `glyph` with its pen at `pen` (pixels, y down,
        // on the baseline), from layout or shaping output. For coverage the
        // quad lands on whole pixels, with the subpixel part of the pen
        // already in the image. For distance fields the quad is scaled from
        // the generation size to `pixel_size`.
        std::expected<atlas_quad, std::string> quad(const font &font, glyph_id glyph, float pixel_size,
                                                    const math::vec2f &pen);

        std::size_t page_count() const noexcept { return m_pages.size(); }
        atlas_page page(std::size_t index) const noexcept;

        // What changed in page `index` since the last call, which forgets it:
        // the region to upload. Empty when nothing did. A new page starts
        // out entirely dirty.
        pixel_rect take_dirty(std::size_t index) noexcept;

        // Forgets every glyph and page. Everything handed out before is
        // invalid, which generation() tells by changing.
        void clear();
        std::uint64_t generation() const noexcept { return m_generation; }

        std::size_t glyph_count() const noexcept { return m_glyphs.size(); }

    private:
        struct key
        {
            std::uint64_t font = 0;
            std::uint32_t size = 0; // 1/64 px
            glyph_id glyph = 0;
            std::uint16_t step = 0;

            friend bool operator==(const key &, const key &) = default;
        };

        struct key_hash
        {
            std::size_t operator()(const key &k) const noexcept;
        };

        struct skyline_node
        {
            int x = 0;
            int y = 0;
            int width = 0;
        };

        struct page_data
        {
            std::vector<std::uint8_t> pixels;
            std::vector<skyline_node> skyline;
            pixel_rect dirty;
        };

        // A free spot for a width x height block in `page`, or false.
        bool place(page_data &page, int width, int height, int &x, int &y);
        std::expected<atlas_glyph, std::string> insert(const font &font, glyph_id glyph, float pixel_size,
                                                       float shift);

        atlas_options m_options;
        std::vector<page_data> m_pages;
        std::unordered_map<key, atlas_glyph, key_hash> m_glyphs;
        std::uint64_t m_generation = 1;
    };

} // namespace volt::font
