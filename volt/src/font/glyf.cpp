#include "face.hpp"

#include <cmath>

// -------------------------------------------------------------------------
// glyf: TrueType outlines
// -------------------------------------------------------------------------
//
// A glyph is either simple, a list of contours of points that are on or off
// the curve (two off-curve points in a row imply an on-curve one halfway
// between), or composite, a list of other glyphs each placed with an offset
// and optionally a 2x2 transform. Composites can nest.
//
// The points are first collected as the file has them, because a composite
// may place a component by matching one of its points to one of the points
// assembled so far, and only then turned into an outline.
//
// Defences against hostile files: nesting deeper than max_depth fails (that
// also ends cycles), and so does a glyph that needs more than max_points
// points or visits more than max_visits components in total, which bounds
// the work for a composite that references a composite that references...

namespace volt::font::detail
{

    namespace
    {
        constexpr int max_depth = 16;
        constexpr std::size_t max_points = 1u << 16;
        constexpr std::uint32_t max_visits = 1u << 14;

        struct points
        {
            std::vector<math::vec2f> positions;
            std::vector<std::uint8_t> on_curve;
            std::vector<std::uint32_t> ends; // one past each contour's last point

            void append(const points &other, const affine &transform)
            {
                const auto base = static_cast<std::uint32_t>(positions.size());
                for (const math::vec2f &p : other.positions)
                    positions.push_back(transform.apply(p));
                on_curve.insert(on_curve.end(), other.on_curve.begin(), other.on_curve.end());
                for (const std::uint32_t end : other.ends)
                    ends.push_back(base + end);
            }
        };

        using result = std::expected<void, std::string>;

        // The glyph's bytes in 'glyf'; empty for a glyph without an outline.
        std::expected<bytes, std::string> locate(const face &face, glyph_id glyph)
        {
            if (glyph >= face.glyph_count)
                return std::unexpected("glyph " + std::to_string(glyph) + " is out of range");

            std::size_t begin = 0;
            std::size_t end = 0;
            if (face.long_loca)
            {
                if (!face.loca.contains((std::size_t{glyph} + 1) * 4, 4))
                    return std::unexpected("'loca' is too short");
                begin = face.loca.u32(std::size_t{glyph} * 4);
                end = face.loca.u32((std::size_t{glyph} + 1) * 4);
            }
            else
            {
                if (!face.loca.contains((std::size_t{glyph} + 1) * 2, 2))
                    return std::unexpected("'loca' is too short");
                begin = std::size_t{face.loca.u16(std::size_t{glyph} * 2)} * 2;
                end = std::size_t{face.loca.u16((std::size_t{glyph} + 1) * 2)} * 2;
            }

            if (end == begin)
                return bytes{};
            if (end < begin || !face.glyf.contains(begin, end - begin))
                return std::unexpected("glyph " + std::to_string(glyph) + " lies outside 'glyf'");
            return face.glyf.sub(begin, end - begin);
        }

        result load_simple(bytes data, int contours, points &out)
        {
            // Each contour's last point index, which must increase.
            std::size_t at = 10;
            if (!data.contains(at, static_cast<std::size_t>(contours) * 2 + 2))
                return std::unexpected("truncated glyph header");
            std::size_t count = 0;
            for (int c = 0; c < contours; ++c)
            {
                const std::size_t last = data.u16(at);
                at += 2;
                if (c > 0 && last + 1 <= count)
                    return std::unexpected("contour ends out of order");
                count = last + 1;
                out.ends.push_back(static_cast<std::uint32_t>(out.positions.size() + count));
            }
            if (out.positions.size() + count > max_points)
                return std::unexpected("too many points");

            // Hinting instructions, unused.
            at += 2 + data.u16(at);

            // Flags, run-length coded: bit 3 says the next byte repeats it.
            std::vector<std::uint8_t> flags;
            flags.reserve(count);
            while (flags.size() < count)
            {
                if (!data.contains(at, 1))
                    return std::unexpected("truncated flags");
                const std::uint8_t flag = data.u8(at++);
                flags.push_back(flag);
                if ((flag & 0x08u) != 0)
                {
                    if (!data.contains(at, 1))
                        return std::unexpected("truncated flags");
                    std::size_t repeat = data.u8(at++);
                    while (repeat-- > 0 && flags.size() < count)
                        flags.push_back(flag);
                }
            }

            // Coordinates are deltas: a byte with a sign flag (short), an
            // int16, or nothing (same as before).
            const std::size_t first = out.positions.size();
            out.positions.resize(first + count);
            const auto coordinates = [&](std::uint8_t short_bit, std::uint8_t same_bit, std::size_t axis) -> result {
                int value = 0;
                for (std::size_t i = 0; i < count; ++i)
                {
                    const std::uint8_t flag = flags[i];
                    if ((flag & short_bit) != 0)
                    {
                        if (!data.contains(at, 1))
                            return std::unexpected("truncated coordinates");
                        const int delta = data.u8(at++);
                        value += (flag & same_bit) != 0 ? delta : -delta;
                    }
                    else if ((flag & same_bit) == 0)
                    {
                        if (!data.contains(at, 2))
                            return std::unexpected("truncated coordinates");
                        value += data.i16(at);
                        at += 2;
                    }
                    out.positions[first + i][axis] = static_cast<float>(value);
                }
                return {};
            };

            if (auto x = coordinates(0x02u, 0x10u, 0); !x)
                return x;
            if (auto y = coordinates(0x04u, 0x20u, 1); !y)
                return y;
            for (std::size_t i = 0; i < count; ++i)
                out.on_curve.push_back(static_cast<std::uint8_t>(flags[i] & 0x01u));
            return {};
        }

        result load(const face &face, glyph_id glyph, int depth, std::uint32_t &visits, points &out);

        result load_composite(const face &face, bytes data, int depth, std::uint32_t &visits, points &out)
        {
            constexpr std::uint16_t words = 0x0001;
            constexpr std::uint16_t xy_values = 0x0002;
            constexpr std::uint16_t has_scale = 0x0008;
            constexpr std::uint16_t more = 0x0020;
            constexpr std::uint16_t has_xy_scale = 0x0040;
            constexpr std::uint16_t has_two_by_two = 0x0080;
            constexpr std::uint16_t scaled_offset = 0x0800;
            constexpr std::uint16_t unscaled_offset = 0x1000;

            const auto f2dot14 = [&](std::size_t offset) {
                return static_cast<float>(data.i16(offset)) / 16384.0f;
            };

            std::size_t at = 10;
            std::uint16_t flags = more;
            while ((flags & more) != 0)
            {
                if (!data.contains(at, 4))
                    return std::unexpected("truncated component");
                flags = data.u16(at);
                const glyph_id component = data.u16(at + 2);
                at += 4;

                int argument_1 = 0;
                int argument_2 = 0;
                if ((flags & words) != 0)
                {
                    if (!data.contains(at, 4))
                        return std::unexpected("truncated component");
                    // Signed offsets, or unsigned point numbers.
                    argument_1 = (flags & xy_values) != 0 ? data.i16(at) : data.u16(at);
                    argument_2 = (flags & xy_values) != 0 ? data.i16(at + 2) : data.u16(at + 2);
                    at += 4;
                }
                else
                {
                    if (!data.contains(at, 2))
                        return std::unexpected("truncated component");
                    const auto signed_byte = [&](std::size_t offset) {
                        return static_cast<int>(static_cast<std::int8_t>(data.u8(offset)));
                    };
                    argument_1 = (flags & xy_values) != 0 ? signed_byte(at) : data.u8(at);
                    argument_2 = (flags & xy_values) != 0 ? signed_byte(at + 1) : data.u8(at + 1);
                    at += 2;
                }

                // The file stores xscale, scale01, scale10, yscale, which map
                // a point as x' = xscale x + scale10 y, y' = scale01 x + yscale y.
                affine transform;
                if ((flags & has_scale) != 0)
                {
                    transform.xx = transform.yy = f2dot14(at);
                    at += 2;
                }
                else if ((flags & has_xy_scale) != 0)
                {
                    transform.xx = f2dot14(at);
                    transform.yy = f2dot14(at + 2);
                    at += 4;
                }
                else if ((flags & has_two_by_two) != 0)
                {
                    transform.xx = f2dot14(at);
                    transform.yx = f2dot14(at + 2);
                    transform.xy = f2dot14(at + 4);
                    transform.yy = f2dot14(at + 6);
                    at += 8;
                }
                if (!data.contains(at, 0))
                    return std::unexpected("truncated component");

                points child;
                if (auto loaded = load(face, component, depth + 1, visits, child); !loaded)
                    return loaded;
                // Transform first, then place: point matching compares
                // the transformed points.
                for (math::vec2f &p : child.positions)
                    p = transform.apply(p);

                math::vec2f offset{};
                if ((flags & xy_values) != 0)
                {
                    offset = {static_cast<float>(argument_1), static_cast<float>(argument_2)};
                    // Apple's convention: the offset goes through the
                    // transform too. Microsoft's (the default) leaves it.
                    if ((flags & scaled_offset) != 0 && (flags & unscaled_offset) == 0)
                    {
                        offset = {transform.xx * offset[0] + transform.xy * offset[1],
                                  transform.yx * offset[0] + transform.yy * offset[1]};
                    }
                }
                else
                {
                    // Put the child's point argument_2 on our point argument_1.
                    const auto ours = static_cast<std::size_t>(argument_1);
                    const auto theirs = static_cast<std::size_t>(argument_2);
                    if (ours >= out.positions.size() || theirs >= child.positions.size())
                        return std::unexpected("component anchored to a point that does not exist");
                    offset = out.positions[ours] - child.positions[theirs];
                }

                if (out.positions.size() + child.positions.size() > max_points)
                    return std::unexpected("too many points");
                out.append(child, affine::scale(1.0f, 1.0f, offset));
            }
            return {};
        }

        result load(const face &face, glyph_id glyph, int depth, std::uint32_t &visits, points &out)
        {
            if (depth > max_depth)
                return std::unexpected("composite glyphs nest too deep (or form a cycle)");
            if (++visits > max_visits)
                return std::unexpected("composite glyph references too many components");

            const auto data = locate(face, glyph);
            if (!data)
                return std::unexpected(data.error());
            if (data->empty())
                return {};
            if (data->size() < 10)
                return std::unexpected("truncated glyph header");

            const int contours = data->i16(0);
            if (contours > 0)
                return load_simple(*data, contours, out);
            if (contours < 0)
                return load_composite(face, *data, depth, visits, out);
            return {};
        }

        math::vec2f midpoint(const math::vec2f &a, const math::vec2f &b) noexcept
        {
            return (a + b) * 0.5f;
        }

        void build(const points &points, outline &out)
        {
            std::size_t begin = 0;
            for (const std::uint32_t contour_end : points.ends)
            {
                const std::size_t end = std::min<std::size_t>(contour_end, points.positions.size());
                const std::size_t count = end > begin ? end - begin : 0;
                const std::size_t first = begin;
                begin = end;
                // A single point is an anchor for composites, not a shape.
                if (count < 2)
                    continue;

                const auto position = [&](std::size_t i) { return points.positions[first + i]; };
                const auto on = [&](std::size_t i) { return points.on_curve[first + i] != 0; };

                // Start on an on-curve point; if there is none at either
                // end, on the implied one between the last and the first.
                std::size_t skip_front = 0;
                std::size_t skip_back = 0;
                math::vec2f start;
                if (on(0))
                {
                    start = position(0);
                    skip_front = 1;
                }
                else if (on(count - 1))
                {
                    start = position(count - 1);
                    skip_back = 1;
                }
                else
                {
                    start = midpoint(position(count - 1), position(0));
                }

                out.move_to(start);
                bool pending = false;
                math::vec2f control;
                for (std::size_t i = skip_front; i < count - skip_back; ++i)
                {
                    const math::vec2f p = position(i);
                    if (on(i))
                    {
                        if (pending)
                            out.quadratic_to(control, p);
                        else
                            out.line_to(p);
                        pending = false;
                    }
                    else
                    {
                        if (pending)
                            out.quadratic_to(control, midpoint(control, p));
                        control = p;
                        pending = true;
                    }
                }
                if (pending)
                    out.quadratic_to(control, start);
                out.close();
            }
        }
    } // namespace

    std::expected<outline, std::string> glyf_outline(const face &face, glyph_id glyph)
    {
        points collected;
        std::uint32_t visits = 0;
        if (auto loaded = load(face, glyph, 0, visits, collected); !loaded)
            return std::unexpected(loaded.error());

        outline result;
        build(collected, result);
        return result;
    }

    box glyf_bounds(const face &face, glyph_id glyph) noexcept
    {
        box result;
        const auto data = locate(face, glyph);
        if (!data || data->size() < 10)
            return result;
        result.add({static_cast<float>(data->i16(2)), static_cast<float>(data->i16(4))});
        result.add({static_cast<float>(data->i16(6)), static_cast<float>(data->i16(8))});
        return result;
    }

} // namespace volt::font::detail
