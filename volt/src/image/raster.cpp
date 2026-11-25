#include <image/raster.hpp>

#include <math/linear_algebra.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace volt::image
{

    namespace
    {
        // The working space a canvas blends in, premultiplied.
        math::vec4f working(math::vec4f color, bool linear)
        {
            float r = std::clamp(color[0], 0.0f, 1.0f);
            float g = std::clamp(color[1], 0.0f, 1.0f);
            float b = std::clamp(color[2], 0.0f, 1.0f);
            const float a = std::clamp(color[3], 0.0f, 1.0f);
            if (linear)
            {
                r = to_linear(srgb_encoding, r);
                g = to_linear(srgb_encoding, g);
                b = to_linear(srgb_encoding, b);
            }
            return {r * a, g * a, b * a, a};
        }

        // ---- coverage accumulation ---------------------------------------------
        //
        // The buffer covers a box of the canvas, `width` + 2 floats a row:
        // edges clipped to the box's right side deposit their area in the
        // columns past it, which no pixel sums up.

        struct accumulator
        {
            float *cells = nullptr;
            std::size_t stride = 0;
            float width = 0.0f;
            float height = 0.0f;

            // An edge already clipped to 0 <= x <= width. After font-rs.
            void line(math::vec2f from, math::vec2f to) const
            {
                if (from[1] == to[1])
                    return;
                float direction = 1.0f;
                if (from[1] > to[1])
                {
                    std::swap(from, to);
                    direction = -1.0f;
                }
                if (to[1] <= 0.0f || from[1] >= height)
                    return;

                const float dxdy = (to[0] - from[0]) / (to[1] - from[1]);
                float x = from[0];
                if (from[1] < 0.0f)
                    x -= from[1] * dxdy;
                const auto first_row = static_cast<std::size_t>(std::max(0.0f, std::floor(from[1])));
                const auto end_row = static_cast<std::size_t>(std::min(height, std::ceil(to[1])));

                for (std::size_t y = first_row; y < end_row; ++y)
                {
                    float *row = cells + y * stride;
                    const float row_top = static_cast<float>(y);
                    const float dy = std::min(row_top + 1.0f, to[1]) - std::max(row_top, from[1]);
                    const float next = x + dxdy * dy;
                    const float d = dy * direction;
                    const float x0 = std::clamp(std::min(x, next), 0.0f, width);
                    const float x1 = std::clamp(std::max(x, next), 0.0f, width);
                    const float x0_floor = std::floor(x0);
                    const auto x0i = static_cast<std::size_t>(x0_floor);
                    const float x1_ceil = std::ceil(x1);
                    const auto x1i = static_cast<std::size_t>(x1_ceil);

                    if (x1i <= x0i + 1)
                    {
                        // Within one column: split by where its middle is.
                        const float middle = 0.5f * (x0 + x1) - x0_floor;
                        row[x0i] += d - d * middle;
                        row[x0i + 1] += d * middle;
                    }
                    else
                    {
                        // Across columns: a trapezoid ramp.
                        const float s = 1.0f / (x1 - x0);
                        const float x0f = x0 - x0_floor;
                        const float a0 = 0.5f * s * (1.0f - x0f) * (1.0f - x0f);
                        const float x1f = x1 - x1_ceil + 1.0f;
                        const float am = 0.5f * s * x1f * x1f;
                        row[x0i] += d * a0;
                        if (x1i == x0i + 2)
                            row[x0i + 1] += d * (1.0f - a0 - am);
                        else
                        {
                            const float a1 = s * (1.5f - x0f);
                            row[x0i + 1] += d * (a1 - a0);
                            for (std::size_t xi = x0i + 2; xi + 1 < x1i; ++xi)
                                row[xi] += d * s;
                            const float a2 = a1 + static_cast<float>(x1i - x0i - 3) * s;
                            row[x1i - 1] += d * (1.0f - a2 - am);
                        }
                        row[x1i] += d * am;
                    }
                    x = next;
                }
            }

            // Any edge: the parts left of the box become vertical edges on
            // its left side (they cover everything to their right), the
            // parts right of it vertical edges on its right side.
            void edge(math::vec2f a, math::vec2f b) const
            {
                math::vec2f points[4] = {a};
                int count = 1;
                for (const float boundary : {0.0f, width})
                {
                    if ((a[0] - boundary) * (b[0] - boundary) < 0.0f)
                    {
                        const float t = (boundary - a[0]) / (b[0] - a[0]);
                        points[count++] = {boundary, a[1] + (b[1] - a[1]) * t};
                    }
                }
                // The crossings in order along the edge.
                if (count == 3 && (points[1][0] - a[0]) * (b[0] - a[0]) > (points[2][0] - a[0]) * (b[0] - a[0]))
                    std::swap(points[1], points[2]);
                points[count++] = b;
                for (int i = 0; i + 1 < count; ++i)
                {
                    math::vec2f p = points[i];
                    math::vec2f q = points[i + 1];
                    p[0] = std::clamp(p[0], 0.0f, width);
                    q[0] = std::clamp(q[0], 0.0f, width);
                    line(p, q);
                }
            }
        };

        // ---- gradients ------------------------------------------------------------

        constexpr int ramp_size = 1024;

        class gradient_sampler
        {
        public:
            gradient_sampler(const gradient &source, const math::mat3f &to_pixels, bool linear) : m_gradient(source)
            {
                // Premultiplied stop colors in the working space, interpolated.
                std::vector<gradient_stop> stops = source.stops;
                float previous = 0.0f;
                for (gradient_stop &stop : stops)
                {
                    stop.offset = std::max(previous, std::clamp(stop.offset, 0.0f, 1.0f));
                    previous = stop.offset;
                    stop.color = working(stop.color, linear);
                }
                m_ramp.resize(ramp_size);
                for (int i = 0; i < ramp_size; ++i)
                {
                    const float t = static_cast<float>(i) / (ramp_size - 1);
                    if (stops.empty())
                    {
                        m_ramp[static_cast<std::size_t>(i)] = {0, 0, 0, 0};
                        continue;
                    }
                    std::size_t k = 0;
                    while (k < stops.size() && stops[k].offset < t)
                        ++k;
                    if (k == 0)
                        m_ramp[static_cast<std::size_t>(i)] = stops.front().color;
                    else if (k == stops.size())
                        m_ramp[static_cast<std::size_t>(i)] = stops.back().color;
                    else
                    {
                        const gradient_stop &a = stops[k - 1];
                        const gradient_stop &b = stops[k];
                        const float span = b.offset - a.offset;
                        const float f = span > 0.0f ? (t - a.offset) / span : 1.0f;
                        m_ramp[static_cast<std::size_t>(i)] = a.color + f * (b.color - a.color);
                    }
                }
                m_last = stops.empty() ? math::vec4f{0, 0, 0, 0} : stops.back().color;

                // Pixels back to gradient coordinates. Degenerate gradients
                // paint with their last color, as SVG says.
                const auto inverse = math::try_inverse(to_pixels * source.transform);
                if (!inverse)
                    m_degenerate = true;
                else
                    m_inverse = *inverse;
                if (source.type == gradient::kind::linear)
                {
                    const math::vec2f d = source.end - source.start;
                    m_length_squared = d[0] * d[0] + d[1] * d[1];
                    m_degenerate = m_degenerate || !(m_length_squared > 0.0f);
                }
                else
                    m_degenerate = m_degenerate || !(source.radius > 0.0f);
            }

            // The premultiplied color at the center of a pixel. Alpha 0 where
            // a radial gradient is not defined.
            math::vec4f at(float x, float y) const
            {
                if (m_degenerate)
                    return m_last;
                const math::vec2f p = apply(m_inverse, {x, y});
                float t = 0.0f;
                if (m_gradient.type == gradient::kind::linear)
                {
                    const math::vec2f d = m_gradient.end - m_gradient.start;
                    const math::vec2f q = p - m_gradient.start;
                    t = (q[0] * d[0] + q[1] * d[1]) / m_length_squared;
                }
                else
                {
                    // |p - f - t (c - f)| = fr + t (r - fr), for the largest t
                    // whose radius is not negative.
                    const math::vec2f cd = m_gradient.end - m_gradient.start;
                    const math::vec2f pd = p - m_gradient.start;
                    const float dr = m_gradient.radius - m_gradient.focal_radius;
                    const float fr = m_gradient.focal_radius;
                    const float a = cd[0] * cd[0] + cd[1] * cd[1] - dr * dr;
                    const float b = pd[0] * cd[0] + pd[1] * cd[1] + fr * dr;
                    const float c = pd[0] * pd[0] + pd[1] * pd[1] - fr * fr;
                    if (std::abs(a) < 1e-9f)
                    {
                        if (std::abs(b) < 1e-12f)
                            return {0, 0, 0, 0};
                        t = c / (2.0f * b);
                    }
                    else
                    {
                        const float discriminant = b * b - a * c;
                        if (discriminant < 0.0f)
                            return {0, 0, 0, 0};
                        const float root = std::sqrt(discriminant);
                        const float t1 = (b + root) / a;
                        const float t2 = (b - root) / a;
                        t = std::max(t1, t2);
                        if (fr + t * dr < 0.0f)
                            t = std::min(t1, t2);
                    }
                    if (fr + t * dr < 0.0f)
                        return {0, 0, 0, 0};
                }

                switch (m_gradient.spread)
                {
                case spread_method::pad:
                    t = std::clamp(t, 0.0f, 1.0f);
                    break;
                case spread_method::repeat:
                    t -= std::floor(t);
                    break;
                case spread_method::reflect:
                    t = std::fmod(std::abs(t), 2.0f);
                    if (t > 1.0f)
                        t = 2.0f - t;
                    break;
                }
                if (!std::isfinite(t))
                    t = 0.0f;
                const auto index = static_cast<std::size_t>(std::clamp(t, 0.0f, 1.0f) * (ramp_size - 1) + 0.5f);
                return m_ramp[index];
            }

        private:
            const gradient &m_gradient;
            std::vector<math::vec4f> m_ramp;
            math::vec4f m_last{};
            math::mat3f m_inverse = math::mat3f::identity();
            float m_length_squared = 0.0f;
            bool m_degenerate = false;
        };
    } // namespace

    std::expected<canvas, std::string> canvas::create(std::uint32_t width, std::uint32_t height, bool linear_light)
    {
        if (auto size = check_size(width, height); !size)
            return std::unexpected(std::move(size.error()));
        if (std::uint64_t{width} * height > max_pixels)
            return std::unexpected(std::format("a canvas of {} x {} pixels is more than the {} allowed", width, height,
                                               max_pixels));
        canvas result;
        result.m_width = width;
        result.m_height = height;
        result.m_linear = linear_light;
        result.m_layers.emplace_back(std::size_t{width} * height * 4, 0.0f);
        return result;
    }

    void canvas::fill(std::span<const contour> polygons, fill_rule rule, const paint &source,
                      const math::mat3f &paint_transform, float opacity)
    {
        if (source.type == paint::kind::none || !(opacity > 0.0f) || polygons.empty())
            return;
        if (source.type == paint::kind::gradient && (!source.gradient || source.gradient->stops.empty()))
            return;

        // The box the polygons touch, within the canvas.
        float min_x = INFINITY;
        float min_y = INFINITY;
        float max_x = -INFINITY;
        float max_y = -INFINITY;
        for (const contour &c : polygons)
            for (const math::vec2f p : c.points)
            {
                if (!std::isfinite(p[0]) || !std::isfinite(p[1]))
                    continue;
                min_x = std::min(min_x, p[0]);
                min_y = std::min(min_y, p[1]);
                max_x = std::max(max_x, p[0]);
                max_y = std::max(max_y, p[1]);
            }
        if (!(min_x <= max_x) || max_x <= 0.0f || max_y <= 0.0f || min_x >= static_cast<float>(m_width) ||
            min_y >= static_cast<float>(m_height))
            return;
        // Everything left of the box still counts (its edges become the
        // box's left side), so the box starts at the polygons' left edge.
        const auto x0 = static_cast<std::uint32_t>(std::max(0.0f, std::floor(min_x)));
        const auto y0 = static_cast<std::uint32_t>(std::max(0.0f, std::floor(min_y)));
        const auto x1 = static_cast<std::uint32_t>(std::min(static_cast<float>(m_width), std::ceil(max_x)));
        const auto y1 = static_cast<std::uint32_t>(std::min(static_cast<float>(m_height), std::ceil(max_y)));
        if (x1 <= x0 || y1 <= y0)
            return;
        const std::size_t box_width = x1 - x0;
        const std::size_t box_height = y1 - y0;
        const std::size_t stride = box_width + 2;

        m_accumulation.assign(stride * box_height, 0.0f);
        const accumulator acc{m_accumulation.data(), stride, static_cast<float>(box_width),
                              static_cast<float>(box_height)};
        const math::vec2f origin{static_cast<float>(x0), static_cast<float>(y0)};
        for (const contour &c : polygons)
        {
            const std::size_t n = c.points.size();
            if (n < 2)
                continue;
            for (std::size_t i = 0; i < n; ++i)
            {
                const math::vec2f a = c.points[i] - origin;
                const math::vec2f b = c.points[(i + 1) % n] - origin;
                if (std::isfinite(a[0]) && std::isfinite(a[1]) && std::isfinite(b[0]) && std::isfinite(b[1]))
                    acc.edge(a, b);
            }
        }

        const math::vec4f solid = working(source.color, m_linear);
        std::unique_ptr<gradient_sampler> sampler;
        if (source.type == paint::kind::gradient)
            sampler = std::make_unique<gradient_sampler>(*source.gradient, paint_transform, m_linear);

        std::vector<float> &pixels = m_layers.back();
        for (std::size_t y = 0; y < box_height; ++y)
        {
            const float *row = m_accumulation.data() + y * stride;
            float *out = pixels.data() + ((y0 + y) * std::size_t{m_width} + x0) * 4;
            float sum = 0.0f;
            for (std::size_t x = 0; x < box_width; ++x, out += 4)
            {
                sum += row[x];
                float coverage = std::abs(sum);
                if (rule == fill_rule::nonzero)
                    coverage = std::min(coverage, 1.0f);
                else
                {
                    coverage = std::fmod(coverage, 2.0f);
                    if (coverage > 1.0f)
                        coverage = 2.0f - coverage;
                }
                if (coverage < 1.0f / 1024.0f)
                    continue;

                const math::vec4f color =
                    sampler ? sampler->at(static_cast<float>(x0 + x) + 0.5f, static_cast<float>(y0 + y) + 0.5f) : solid;
                const float alpha = coverage * opacity;
                const float keep = 1.0f - color[3] * alpha;
                out[0] = color[0] * alpha + out[0] * keep;
                out[1] = color[1] * alpha + out[1] * keep;
                out[2] = color[2] * alpha + out[2] * keep;
                out[3] = color[3] * alpha + out[3] * keep;
            }
        }
    }

    void canvas::push_layer()
    {
        if (m_layers.size() > max_layers)
        {
            ++m_ignored_layers;
            return;
        }
        m_layers.emplace_back(std::size_t{m_width} * m_height * 4, 0.0f);
    }

    void canvas::pop_layer(float opacity)
    {
        if (m_ignored_layers > 0)
        {
            --m_ignored_layers;
            return;
        }
        if (m_layers.size() < 2)
            return;
        const std::vector<float> top = std::move(m_layers.back());
        m_layers.pop_back();
        std::vector<float> &below = m_layers.back();
        opacity = std::clamp(opacity, 0.0f, 1.0f);
        for (std::size_t i = 0; i < below.size(); i += 4)
        {
            const float keep = 1.0f - top[i + 3] * opacity;
            for (std::size_t c = 0; c < 4; ++c)
                below[i + c] = top[i + c] * opacity + below[i + c] * keep;
        }
    }

    image canvas::to_image() const
    {
        auto created = image::create(m_width, m_height, pixel_format::rgba8);
        image result = std::move(*created);
        const std::vector<float> &pixels = m_layers.front();
        std::uint8_t *out = result.data();
        for (std::size_t i = 0; i < pixels.size(); i += 4)
        {
            const float a = std::clamp(pixels[i + 3], 0.0f, 1.0f);
            for (std::size_t c = 0; c < 3; ++c)
            {
                float value = a > 0.0f ? std::clamp(pixels[i + c] / a, 0.0f, 1.0f) : 0.0f;
                if (m_linear)
                    value = from_linear(srgb_encoding, value);
                out[i + c] = static_cast<std::uint8_t>(value * 255.0f + 0.5f);
            }
            out[i + 3] = static_cast<std::uint8_t>(a * 255.0f + 0.5f);
        }
        return result;
    }

} // namespace volt::image
