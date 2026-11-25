#include <font/raster.hpp>

#include "pixel_box.hpp"

#include <algorithm>
#include <cmath>

// -------------------------------------------------------------------------
// Signed distance fields
// -------------------------------------------------------------------------
//
// Computed analytically against the outline flattened very finely, not
// estimated from a high resolution raster: per pixel center, the distance to
// the nearest edge, and the side from the nonzero winding number there.
//
// Brute force over edges, with two cuts that make it cheap at glyph sizes:
// distances beyond the spread all saturate, so each row only looks at edges
// within the spread of it vertically, and each pixel skips edges whose box
// is already farther than the best distance found.

namespace volt::font
{

    namespace
    {
        struct edge
        {
            float ax, ay, bx, by;
            float min_x, max_x, min_y, max_y;
        };

        float distance_squared(const edge &e, float x, float y) noexcept
        {
            const float dx = e.bx - e.ax;
            const float dy = e.by - e.ay;
            const float length = dx * dx + dy * dy;
            float t = 0.0f;
            if (length > 0.0f)
                t = std::clamp(((x - e.ax) * dx + (y - e.ay) * dy) / length, 0.0f, 1.0f);
            const float px = e.ax + t * dx - x;
            const float py = e.ay + t * dy - y;
            return px * px + py * py;
        }
    } // namespace

    std::expected<glyph_image, std::string> distance_field(const outline &shape, float scale, float spread)
    {
        if (!(spread > 0.0f) || spread > 256.0f)
            return std::unexpected("the spread must be between 0 and 256 pixels");

        glyph_image image;
        math::vec2i origin{};
        math::vec2i size{};
        const auto any = detail::pixel_box(shape, scale, 0.0f, std::ceil(spread), origin, size);
        if (!any)
            return std::unexpected(any.error());
        if (!*any)
            return image;

        const polygon flat =
            shape
                .transformed(affine::scale(scale, -scale,
                                           {-static_cast<float>(origin[0]), -static_cast<float>(origin[1])}))
                .flatten(1.0f / 64.0f);

        std::vector<edge> edges;
        edges.reserve(flat.points.size());
        for (std::size_t c = 0; c < flat.contour_count(); ++c)
        {
            const std::span<const math::vec2f> points = flat.contour(c);
            for (std::size_t i = 0; i < points.size(); ++i)
            {
                const math::vec2f a = points[i];
                const math::vec2f b = points[i + 1 < points.size() ? i + 1 : 0];
                edges.push_back({a[0], a[1], b[0], b[1], std::min(a[0], b[0]), std::max(a[0], b[0]),
                                 std::min(a[1], b[1]), std::max(a[1], b[1])});
            }
        }

        image.width = size[0];
        image.height = size[1];
        image.offset = origin;
        image.pixels.assign(static_cast<std::size_t>(size[0]) * static_cast<std::size_t>(size[1]), 0);

        struct crossing
        {
            float x;
            int winding;
        };
        std::vector<crossing> crossings;
        std::vector<const edge *> near;
        const float limit = spread * spread;

        for (int y = 0; y < size[1]; ++y)
        {
            const float center_y = static_cast<float>(y) + 0.5f;

            // Where the row's center line crosses the outline, and which
            // way: walking along it, the winding number changes there.
            crossings.clear();
            near.clear();
            for (const edge &e : edges)
            {
                if (e.ay != e.by && center_y >= e.min_y && center_y < e.max_y)
                {
                    const float t = (center_y - e.ay) / (e.by - e.ay);
                    crossings.push_back({e.ax + t * (e.bx - e.ax), e.ay < e.by ? 1 : -1});
                }
                if (center_y >= e.min_y - spread && center_y <= e.max_y + spread)
                    near.push_back(&e);
            }
            std::sort(crossings.begin(), crossings.end(),
                      [](const crossing &a, const crossing &b) { return a.x < b.x; });

            std::size_t next = 0;
            int winding = 0;
            std::uint8_t *row = image.pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(size[0]);
            for (int x = 0; x < size[0]; ++x)
            {
                const float center_x = static_cast<float>(x) + 0.5f;
                while (next < crossings.size() && crossings[next].x < center_x)
                    winding += crossings[next++].winding;

                float best = limit;
                for (const edge *e : near)
                {
                    const float out_x = std::max({e->min_x - center_x, center_x - e->max_x, 0.0f});
                    const float out_y = std::max({e->min_y - center_y, center_y - e->max_y, 0.0f});
                    if (out_x * out_x + out_y * out_y >= best)
                        continue;
                    best = std::min(best, distance_squared(*e, center_x, center_y));
                }

                const float distance = std::sqrt(best);
                const float signed_distance = winding != 0 ? distance : -distance;
                const float value = std::clamp(0.5f + signed_distance / (2.0f * spread), 0.0f, 1.0f);
                row[x] = static_cast<std::uint8_t>(value * 255.0f + 0.5f);
            }
        }
        return image;
    }

    std::expected<glyph_image, std::string> distance_field(const font &font, glyph_id glyph, float pixel_size,
                                                           float spread)
    {
        const auto shape = font.glyph_outline(glyph);
        if (!shape)
            return std::unexpected(shape.error());
        return distance_field(*shape, font.scale(pixel_size), spread);
    }

} // namespace volt::font
