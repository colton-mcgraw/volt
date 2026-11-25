#include <font/raster.hpp>

#include "pixel_box.hpp"

#include <algorithm>
#include <cmath>

// -------------------------------------------------------------------------
// Coverage by signed area accumulation
// -------------------------------------------------------------------------
//
// The approach of font-rs and stb_truetype 2: every edge deposits, into the
// cells of each row it crosses, the signed area between itself and the
// cell's right side, and how much of the row's height it spans to every
// cell further right. A running sum along the row then gives, per pixel, the
// exact signed area of the polygon inside it. The sign comes from the edge's
// direction, so a hole drawn the other way cancels, and the absolute value
// clamped to 1 is the nonzero rule (exact unless contours overlap inside a
// single pixel).
//
// Each row of cells is two wider than the canvas: an edge on the right
// border deposits into the cell after the last pixel, and the running sum
// never reads it.

namespace volt::font
{

    void rasterizer::reset(int width, int height)
    {
        m_width = std::clamp(width, 0, max_image_size);
        m_height = std::clamp(height, 0, max_image_size);
        m_stride = static_cast<std::size_t>(m_width) + 2;
        m_cells.assign(m_stride * static_cast<std::size_t>(m_height), 0.0f);
    }

    void rasterizer::line(const math::vec2f &from, const math::vec2f &to) noexcept
    {
        float x0 = from[0];
        float y0 = from[1];
        float x1 = to[0];
        float y1 = to[1];
        if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) || !std::isfinite(y1))
            return;
        if (y0 == y1 || m_width == 0 || m_height == 0)
            return;

        // Walk downwards; the direction lives on as the sign.
        float direction = 1.0f;
        if (y0 > y1)
        {
            std::swap(x0, x1);
            std::swap(y0, y1);
            direction = -1.0f;
        }

        const auto height = static_cast<float>(m_height);
        if (y1 <= 0.0f || y0 >= height)
            return;

        const float dxdy = (x1 - x0) / (y1 - y0);
        float x = x0;
        if (y0 < 0.0f)
        {
            x -= y0 * dxdy;
            y0 = 0.0f;
        }
        y1 = std::min(y1, height);

        const auto width = static_cast<float>(m_width);
        const int first_row = static_cast<int>(y0);
        const int last_row = std::min(static_cast<int>(std::ceil(y1)), m_height);
        for (int y = first_row; y < last_row; ++y)
        {
            const float top = std::max(static_cast<float>(y), y0);
            const float bottom = std::min(static_cast<float>(y + 1), y1);
            const float dy = bottom - top;
            const float x_next = x + dxdy * dy;
            const float d = dy * direction;

            float left = std::clamp(std::min(x, x_next), 0.0f, width);
            float right = std::clamp(std::max(x, x_next), 0.0f, width);
            x = x_next;

            float *row = m_cells.data() + static_cast<std::size_t>(y) * m_stride;
            const float left_floor = std::floor(left);
            const auto left_cell = static_cast<std::size_t>(left_floor);
            const float right_ceil = std::ceil(right);
            const auto right_cell = static_cast<std::size_t>(right_ceil);

            if (right_cell <= left_cell + 1)
            {
                // Within one column: the part of the cell right of the edge
                // is its covered share, the rest carries to the next cell.
                const float middle = 0.5f * (left + right) - left_floor;
                row[left_cell] += d - d * middle;
                row[left_cell + 1] += d * middle;
                continue;
            }

            // Across several columns: the edge sweeps a triangle in the
            // first cell, trapezoids through the middle, a triangle in the
            // last, each proportional to its share of the width.
            const float inverse = 1.0f / (right - left);
            const float left_fraction = left - left_floor;
            const float first = 0.5f * inverse * (1.0f - left_fraction) * (1.0f - left_fraction);
            const float right_fraction = right - right_ceil + 1.0f;
            const float last = 0.5f * inverse * right_fraction * right_fraction;
            row[left_cell] += d * first;
            if (right_cell == left_cell + 2)
            {
                row[left_cell + 1] += d * (1.0f - first - last);
            }
            else
            {
                const float second = inverse * (1.5f - left_fraction);
                row[left_cell + 1] += d * (second - first);
                for (std::size_t cell = left_cell + 2; cell + 1 < right_cell; ++cell)
                    row[cell] += d * inverse;
                const float before_last =
                    second + static_cast<float>(right_cell - left_cell - 3) * inverse;
                row[right_cell - 1] += d * (1.0f - before_last - last);
            }
            row[right_cell] += d * last;
        }
    }

    void rasterizer::add(const outline &shape, const affine &transform, float tolerance)
    {
        // Flatten in pixels, so that the tolerance is in pixels.
        shape.transformed(transform).flatten(tolerance, m_scratch);
        for (std::size_t c = 0; c < m_scratch.contour_count(); ++c)
        {
            const std::span<const math::vec2f> points = m_scratch.contour(c);
            for (std::size_t i = 0; i < points.size(); ++i)
                line(points[i], points[i + 1 < points.size() ? i + 1 : 0]);
        }
    }

    void rasterizer::resolve(std::span<std::uint8_t> out, std::size_t stride) const noexcept
    {
        const auto width = static_cast<std::size_t>(m_width);
        if (stride < width || out.size() < stride * static_cast<std::size_t>(m_height))
            return;
        for (std::size_t y = 0; y < static_cast<std::size_t>(m_height); ++y)
        {
            const float *row = m_cells.data() + y * m_stride;
            std::uint8_t *target = out.data() + y * stride;
            float sum = 0.0f;
            for (std::size_t x = 0; x < width; ++x)
            {
                sum += row[x];
                const float coverage = std::min(std::abs(sum), 1.0f);
                const auto value = static_cast<std::uint8_t>(coverage * 255.0f + 0.5f);
                target[x] = std::max(target[x], value);
            }
        }
    }

    void rasterizer::resolve(std::span<float> out) const noexcept
    {
        const auto width = static_cast<std::size_t>(m_width);
        if (out.size() < width * static_cast<std::size_t>(m_height))
            return;
        for (std::size_t y = 0; y < static_cast<std::size_t>(m_height); ++y)
        {
            const float *row = m_cells.data() + y * m_stride;
            float sum = 0.0f;
            for (std::size_t x = 0; x < width; ++x)
            {
                sum += row[x];
                out[y * width + x] = std::abs(sum);
            }
        }
    }

    // ---------------------------------------------------------------------
    // Glyph images
    // ---------------------------------------------------------------------


    std::expected<glyph_image, std::string> rasterize(const outline &shape, float scale, float shift)
    {
        glyph_image image;
        math::vec2i origin{};
        math::vec2i size{};
        const auto any = detail::pixel_box(shape, scale, shift, 0.0f, origin, size);
        if (!any)
            return std::unexpected(any.error());
        if (!*any)
            return image;

        // Reused per thread: glyphs are rasterized one after the other.
        thread_local rasterizer canvas;
        canvas.reset(size[0], size[1]);
        canvas.add(shape, affine::scale(scale, -scale,
                                        {shift - static_cast<float>(origin[0]), -static_cast<float>(origin[1])}),
                   raster_tolerance);

        image.width = size[0];
        image.height = size[1];
        image.offset = origin;
        image.pixels.assign(static_cast<std::size_t>(size[0]) * static_cast<std::size_t>(size[1]), 0);
        canvas.resolve(image.pixels, static_cast<std::size_t>(size[0]));
        return image;
    }

    std::expected<glyph_image, std::string> rasterize(const font &font, glyph_id glyph, float pixel_size,
                                                      float shift)
    {
        const auto shape = font.glyph_outline(glyph);
        if (!shape)
            return std::unexpected(shape.error());
        return rasterize(*shape, font.scale(pixel_size), shift);
    }


} // namespace volt::font
