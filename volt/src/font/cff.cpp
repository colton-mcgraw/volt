#include "face.hpp"

#include <array>
#include <cmath>

// -------------------------------------------------------------------------
// CFF: OpenType fonts with PostScript outlines
// -------------------------------------------------------------------------
//
// The 'CFF ' table is a font program of its own: a header, INDEXes (arrays
// of variable-length items) of names, top-level dictionaries, strings and
// global subroutines; then, found through the top dictionary, the glyphs'
// charstrings and a private dictionary with the local subroutines. A
// CID-keyed font has a private dictionary per font dict (FDArray) and maps
// glyphs to them (FDSelect).
//
// Charstrings are Type 2: a small stack machine whose operators draw lines
// and cubic curves relative to the current point, declare hints (skipped
// here, only counted, because a hint mask's length depends on them), and
// call subroutines. Calls nest at most 10 deep, as the format says, and a
// glyph may run at most max_operations operators in total, so that no
// charstring can hang the interpreter.
//
// Not supported: CFF2 (variable CFF), and the deprecated seac form of
// endchar, which builds an accented glyph from two standard-encoded ones;
// such a glyph comes out as its base only. The font matrix is assumed to be
// the usual one, font units = 1 / unitsPerEm.

namespace volt::font::detail
{

    namespace
    {
        constexpr int max_call_depth = 10;
        constexpr int max_stack = 48;
        constexpr std::uint32_t max_operations = 1u << 16;

        // ---- INDEX and DICT ----------------------------------------------

        std::expected<cff_index, std::string> read_index(bytes table, std::size_t at)
        {
            cff_index index;
            index.data = table;
            if (!table.contains(at, 2))
                return std::unexpected("truncated CFF INDEX");
            index.count = table.u16(at);
            if (index.count == 0)
            {
                index.end = at + 2;
                return index;
            }
            index.offset_size = table.u8(at + 2);
            if (index.offset_size < 1 || index.offset_size > 4)
                return std::unexpected("bad CFF INDEX offset size");
            index.offsets = at + 3;
            const std::size_t offsets_length = (std::size_t{index.count} + 1) * index.offset_size;
            if (!table.contains(index.offsets, offsets_length))
                return std::unexpected("truncated CFF INDEX");
            index.base = index.offsets + offsets_length - 1;
            const std::uint32_t last = table.uint(index.offsets + std::size_t{index.count} * index.offset_size,
                                                  index.offset_size);
            if (last == 0 || !table.contains(index.base + 1, last - 1))
                return std::unexpected("CFF INDEX data runs past the table");
            index.end = index.base + last;
            return index;
        }

        // The operands of one DICT operator, found by scanning the DICT.
        struct dict_entry
        {
            std::array<double, 4> operands{};
            int count = 0;
            bool found = false;
        };

        // Looks up `op` (12 x is 1200 + x) in a DICT.
        dict_entry find_operator(bytes dict, int op)
        {
            dict_entry entry;
            std::array<double, max_stack> stack{};
            int depth = 0;
            std::size_t at = 0;
            while (at < dict.size())
            {
                const std::uint8_t b0 = dict.u8(at);
                double value = 0.0;
                if (b0 <= 21)
                {
                    int code = b0;
                    ++at;
                    if (b0 == 12)
                        code = 1200 + dict.u8(at++);
                    if (code == op)
                    {
                        entry.found = true;
                        entry.count = std::min(depth, 4);
                        for (int i = 0; i < entry.count; ++i)
                            entry.operands[static_cast<std::size_t>(i)] = stack[static_cast<std::size_t>(depth - entry.count + i)];
                        return entry;
                    }
                    depth = 0;
                    continue;
                }
                if (b0 == 28)
                {
                    value = dict.i16(at + 1);
                    at += 3;
                }
                else if (b0 == 29)
                {
                    value = dict.i32(at + 1);
                    at += 5;
                }
                else if (b0 == 30)
                {
                    // A real, as nibbles. Only its extent matters for the
                    // operators used here (offsets and sizes are integers).
                    ++at;
                    while (at < dict.size())
                    {
                        const std::uint8_t b = dict.u8(at++);
                        if ((b & 0x0Fu) == 0x0Fu || (b >> 4) == 0x0Fu)
                            break;
                    }
                }
                else if (b0 >= 32 && b0 <= 246)
                {
                    value = b0 - 139;
                    at += 1;
                }
                else if (b0 >= 247 && b0 <= 250)
                {
                    value = (b0 - 247) * 256 + dict.u8(at + 1) + 108;
                    at += 2;
                }
                else if (b0 >= 251 && b0 <= 254)
                {
                    value = -(b0 - 251) * 256 - dict.u8(at + 1) - 108;
                    at += 2;
                }
                else
                {
                    ++at; // reserved
                    continue;
                }
                if (depth < max_stack)
                    stack[static_cast<std::size_t>(depth++)] = value;
            }
            return entry;
        }

        bool valid_offset(double value, std::size_t size) noexcept
        {
            return value >= 0.0 && value < static_cast<double>(size);
        }

        // The local subroutines of a font or font dict, through its Private
        // DICT. An empty INDEX when there are none.
        std::expected<cff_index, std::string> read_local_subrs(bytes table, bytes font_dict)
        {
            cff_index none;
            none.data = table;
            const dict_entry private_dict = find_operator(font_dict, 18);
            if (!private_dict.found || private_dict.count < 2)
                return none;
            const double size = private_dict.operands[0];
            const double offset = private_dict.operands[1];
            if (!valid_offset(offset, table.size()) || size < 0.0)
                return std::unexpected("bad CFF Private DICT");
            const auto at = static_cast<std::size_t>(offset);
            const bytes dict = table.sub(at, static_cast<std::size_t>(size));
            const dict_entry subrs = find_operator(dict, 19);
            if (!subrs.found || subrs.count < 1)
                return none;
            const double subrs_at = offset + subrs.operands[0];
            if (!valid_offset(subrs_at, table.size()))
                return std::unexpected("bad CFF Subrs offset");
            return read_index(table, static_cast<std::size_t>(subrs_at));
        }

        // ---- charstrings ---------------------------------------------------

        std::int32_t bias(std::uint32_t count) noexcept
        {
            if (count < 1240)
                return 107;
            if (count < 33900)
                return 1131;
            return 32768;
        }

        class interpreter
        {
        public:
            interpreter(const cff_index &global, const cff_index &local, outline &out)
                : m_global(global), m_local(local), m_out(out)
            {
            }

            std::expected<void, std::string> run(bytes program, int depth)
            {
                if (depth > max_call_depth)
                    return std::unexpected("CFF subroutines nest too deep");

                std::size_t at = 0;
                while (at < program.size())
                {
                    if (++m_operations > max_operations)
                        return std::unexpected("CFF charstring runs too long");

                    const std::uint8_t b0 = program.u8(at);
                    // Operands.
                    if (b0 >= 32 || b0 == 28)
                    {
                        float value = 0.0f;
                        if (b0 == 28)
                        {
                            value = program.i16(at + 1);
                            at += 3;
                        }
                        else if (b0 <= 246)
                        {
                            value = static_cast<float>(b0 - 139);
                            at += 1;
                        }
                        else if (b0 <= 250)
                        {
                            value = static_cast<float>((b0 - 247) * 256 + program.u8(at + 1) + 108);
                            at += 2;
                        }
                        else if (b0 <= 254)
                        {
                            value = static_cast<float>(-(b0 - 251) * 256 - program.u8(at + 1) - 108);
                            at += 2;
                        }
                        else
                        {
                            value = static_cast<float>(program.i32(at + 1)) / 65536.0f; // 16.16
                            at += 5;
                        }
                        if (m_depth >= max_stack)
                            return std::unexpected("CFF stack overflow");
                        m_stack[static_cast<std::size_t>(m_depth++)] = value;
                        continue;
                    }

                    ++at;
                    switch (b0)
                    {
                    case 1:  // hstem
                    case 3:  // vstem
                    case 18: // hstemhm
                    case 23: // vstemhm
                        take_width(m_depth % 2 != 0);
                        m_stems += m_depth / 2;
                        clear();
                        break;

                    case 19: // hintmask
                    case 20: // cntrmask
                        // Operands before a mask are an implied vstem.
                        take_width(m_depth % 2 != 0);
                        m_stems += m_depth / 2;
                        clear();
                        at += static_cast<std::size_t>((m_stems + 7) / 8);
                        break;

                    case 21: // rmoveto
                        take_width(m_depth > 2);
                        if (m_depth < 2)
                            return underflow();
                        move(arg(0), arg(1));
                        break;
                    case 22: // hmoveto
                        take_width(m_depth > 1);
                        if (m_depth < 1)
                            return underflow();
                        move(arg(0), 0.0f);
                        break;
                    case 4: // vmoveto
                        take_width(m_depth > 1);
                        if (m_depth < 1)
                            return underflow();
                        move(0.0f, arg(0));
                        break;

                    case 5: // rlineto
                        for (int i = 0; i + 1 < m_depth; i += 2)
                            line(arg(i), arg(i + 1));
                        clear();
                        break;
                    case 6: // hlineto
                    case 7: // vlineto
                    {
                        bool horizontal = b0 == 6;
                        for (int i = 0; i < m_depth; ++i, horizontal = !horizontal)
                        {
                            if (horizontal)
                                line(arg(i), 0.0f);
                            else
                                line(0.0f, arg(i));
                        }
                        clear();
                        break;
                    }

                    case 8: // rrcurveto
                        for (int i = 0; i + 5 < m_depth; i += 6)
                            curve(arg(i), arg(i + 1), arg(i + 2), arg(i + 3), arg(i + 4), arg(i + 5));
                        clear();
                        break;
                    case 24: // rcurveline
                    {
                        int i = 0;
                        for (; i + 7 < m_depth; i += 6)
                            curve(arg(i), arg(i + 1), arg(i + 2), arg(i + 3), arg(i + 4), arg(i + 5));
                        if (i + 1 < m_depth)
                            line(arg(i), arg(i + 1));
                        clear();
                        break;
                    }
                    case 25: // rlinecurve
                    {
                        int i = 0;
                        for (; i + 7 < m_depth; i += 2)
                            line(arg(i), arg(i + 1));
                        if (i + 5 < m_depth)
                            curve(arg(i), arg(i + 1), arg(i + 2), arg(i + 3), arg(i + 4), arg(i + 5));
                        clear();
                        break;
                    }
                    case 26: // vvcurveto
                    {
                        int i = 0;
                        float dx1 = 0.0f;
                        if (m_depth % 2 != 0)
                            dx1 = arg(i++);
                        for (; i + 3 < m_depth; i += 4, dx1 = 0.0f)
                            curve(dx1, arg(i), arg(i + 1), arg(i + 2), 0.0f, arg(i + 3));
                        clear();
                        break;
                    }
                    case 27: // hhcurveto
                    {
                        int i = 0;
                        float dy1 = 0.0f;
                        if (m_depth % 2 != 0)
                            dy1 = arg(i++);
                        for (; i + 3 < m_depth; i += 4, dy1 = 0.0f)
                            curve(arg(i), dy1, arg(i + 1), arg(i + 2), arg(i + 3), 0.0f);
                        clear();
                        break;
                    }
                    case 30: // vhcurveto
                    case 31: // hvcurveto
                    {
                        // Curves that start alternately horizontal and
                        // vertical; the last may take a fifth argument for
                        // the coordinate its end would otherwise keep.
                        bool horizontal = b0 == 31;
                        for (int i = 0; i + 3 < m_depth; i += 4, horizontal = !horizontal)
                        {
                            const float last = m_depth - i == 5 ? arg(i + 4) : 0.0f;
                            if (horizontal)
                                curve(arg(i), 0.0f, arg(i + 1), arg(i + 2), last, arg(i + 3));
                            else
                                curve(0.0f, arg(i), arg(i + 1), arg(i + 2), arg(i + 3), last);
                        }
                        clear();
                        break;
                    }

                    case 10: // callsubr
                    case 29: // callgsubr
                    {
                        if (m_depth < 1)
                            return underflow();
                        const cff_index &subrs = b0 == 10 ? m_local : m_global;
                        const auto number = static_cast<std::int64_t>(arg(--m_depth)) + bias(subrs.count);
                        if (number < 0 || number >= static_cast<std::int64_t>(subrs.count))
                            return std::unexpected("CFF subroutine out of range");
                        if (auto called = run(subrs.item(static_cast<std::uint32_t>(number)), depth + 1); !called)
                            return called;
                        if (m_ended)
                            return {};
                        break;
                    }
                    case 11: // return
                        return {};
                    case 14: // endchar
                        // With four operands, the deprecated seac; drawn as
                        // the base only (see the top of the file).
                        take_width(m_depth == 1 || m_depth == 5);
                        m_out.close();
                        m_ended = true;
                        return {};

                    case 12:
                    {
                        const std::uint8_t b1 = program.u8(at++);
                        if (auto done = escape(b1); !done)
                            return done;
                        break;
                    }

                    default:
                        return std::unexpected("unknown CFF operator " + std::to_string(b0));
                    }
                }
                return {};
            }

            bool ended() const noexcept { return m_ended; }

        private:
            std::expected<void, std::string> escape(std::uint8_t op)
            {
                switch (op)
                {
                case 35: // flex
                    if (m_depth < 13)
                        return underflow();
                    curve(arg(0), arg(1), arg(2), arg(3), arg(4), arg(5));
                    curve(arg(6), arg(7), arg(8), arg(9), arg(10), arg(11));
                    clear();
                    return {};
                case 34: // hflex
                    if (m_depth < 7)
                        return underflow();
                    curve(arg(0), 0.0f, arg(1), arg(2), arg(3), 0.0f);
                    curve(arg(4), 0.0f, arg(5), -arg(2), arg(6), 0.0f);
                    clear();
                    return {};
                case 36: // hflex1
                {
                    if (m_depth < 9)
                        return underflow();
                    // dx1 dy1 dx2 dy2 dx3, then dx4 dx5 dy5 dx6: the second
                    // curve starts level and ends back at the starting height.
                    const float start_y = m_y;
                    curve(arg(0), arg(1), arg(2), arg(3), arg(4), 0.0f);
                    curve(arg(5), 0.0f, arg(6), arg(7), arg(8), start_y - (m_y + arg(7)));
                    clear();
                    return {};
                }
                case 37: // flex1
                {
                    if (m_depth < 11)
                        return underflow();
                    float dx = 0.0f;
                    float dy = 0.0f;
                    for (int i = 0; i < 10; i += 2)
                    {
                        dx += arg(i);
                        dy += arg(i + 1);
                    }
                    curve(arg(0), arg(1), arg(2), arg(3), arg(4), arg(5));
                    // The last point moves along whichever axis the flex
                    // travelled further in; the other returns to the start.
                    if (std::abs(dx) > std::abs(dy))
                        curve(arg(6), arg(7), arg(8), arg(9), arg(10), -dy);
                    else
                        curve(arg(6), arg(7), arg(8), arg(9), -dx, arg(10));
                    clear();
                    return {};
                }

                // Arithmetic: rare in real fonts, cheap to have.
                case 9: // abs
                    return unary([](float a) { return std::abs(a); });
                case 14: // neg
                    return unary([](float a) { return -a; });
                case 26: // sqrt
                    return unary([](float a) { return a > 0.0f ? std::sqrt(a) : 0.0f; });
                case 10: // add
                    return binary([](float a, float b) { return a + b; });
                case 11: // sub
                    return binary([](float a, float b) { return a - b; });
                case 24: // mul
                    return binary([](float a, float b) { return a * b; });
                case 12: // div
                    return binary([](float a, float b) { return b != 0.0f ? a / b : 0.0f; });
                case 18: // drop
                    if (m_depth < 1)
                        return underflow();
                    --m_depth;
                    return {};
                case 27: // dup
                    if (m_depth < 1)
                        return underflow();
                    if (m_depth >= max_stack)
                        return std::unexpected("CFF stack overflow");
                    m_stack[static_cast<std::size_t>(m_depth)] = m_stack[static_cast<std::size_t>(m_depth - 1)];
                    ++m_depth;
                    return {};
                case 28: // exch
                    if (m_depth < 2)
                        return underflow();
                    std::swap(m_stack[static_cast<std::size_t>(m_depth - 1)], m_stack[static_cast<std::size_t>(m_depth - 2)]);
                    return {};
                default:
                    return std::unexpected("unsupported CFF operator 12 " + std::to_string(op));
                }
            }

            template <typename F>
            std::expected<void, std::string> unary(F f)
            {
                if (m_depth < 1)
                    return underflow();
                m_stack[static_cast<std::size_t>(m_depth - 1)] = f(m_stack[static_cast<std::size_t>(m_depth - 1)]);
                return {};
            }

            template <typename F>
            std::expected<void, std::string> binary(F f)
            {
                if (m_depth < 2)
                    return underflow();
                const float b = m_stack[static_cast<std::size_t>(--m_depth)];
                m_stack[static_cast<std::size_t>(m_depth - 1)] = f(m_stack[static_cast<std::size_t>(m_depth - 1)], b);
                return {};
            }

            static std::unexpected<std::string> underflow()
            {
                return std::unexpected(std::string("CFF stack underflow"));
            }

            // The first stack-clearing operator may carry the advance width
            // as an extra first operand. It is not needed ('hmtx' has the
            // advances), only removed.
            void take_width(bool present) noexcept
            {
                if (m_width_done)
                    return;
                m_width_done = true;
                if (present && m_depth > 0)
                {
                    for (int i = 1; i < m_depth; ++i)
                        m_stack[static_cast<std::size_t>(i - 1)] = m_stack[static_cast<std::size_t>(i)];
                    --m_depth;
                }
            }

            float arg(int index) const noexcept { return m_stack[static_cast<std::size_t>(index)]; }
            void clear() noexcept { m_depth = 0; }

            void move(float dx, float dy)
            {
                m_x += dx;
                m_y += dy;
                m_out.move_to({m_x, m_y});
                clear();
            }

            void line(float dx, float dy)
            {
                m_x += dx;
                m_y += dy;
                m_out.line_to({m_x, m_y});
            }

            // Three relative steps: to the first control point, from it to
            // the second, from that to the end.
            void curve(float dx1, float dy1, float dx2, float dy2, float dx3, float dy3)
            {
                const math::vec2f a{m_x + dx1, m_y + dy1};
                const math::vec2f b{a[0] + dx2, a[1] + dy2};
                m_x = b[0] + dx3;
                m_y = b[1] + dy3;
                m_out.cubic_to(a, b, {m_x, m_y});
            }

            const cff_index &m_global;
            const cff_index &m_local;
            outline &m_out;

            std::array<float, max_stack> m_stack{};
            int m_depth = 0;
            int m_stems = 0;
            float m_x = 0.0f;
            float m_y = 0.0f;
            std::uint32_t m_operations = 0;
            bool m_width_done = false;
            bool m_ended = false;
        };
    } // namespace

    std::expected<void, std::string> parse_cff(face &face)
    {
        const bytes table = face.cff;
        if (table.size() < 4 || table.u8(0) != 1)
            return std::unexpected("unsupported CFF version");

        const auto names = read_index(table, table.u8(2));
        if (!names)
            return std::unexpected(names.error());
        const auto top_dicts = read_index(table, names->end);
        if (!top_dicts)
            return std::unexpected(top_dicts.error());
        const auto strings = read_index(table, top_dicts->end);
        if (!strings)
            return std::unexpected(strings.error());
        auto global = read_index(table, strings->end);
        if (!global)
            return std::unexpected(global.error());
        if (top_dicts->count < 1)
            return std::unexpected("the CFF table has no font");

        cff_font &cff = face.cff_data;
        cff.global_subrs = *global;
        const bytes top = top_dicts->item(0);

        const dict_entry type = find_operator(top, 1206);
        if (type.found && type.count > 0 && type.operands[0] != 2.0)
            return std::unexpected("only Type 2 charstrings are supported");

        const dict_entry charstrings = find_operator(top, 17);
        if (!charstrings.found || charstrings.count < 1 || !valid_offset(charstrings.operands[0], table.size()))
            return std::unexpected("the CFF font has no CharStrings");
        auto glyphs = read_index(table, static_cast<std::size_t>(charstrings.operands[0]));
        if (!glyphs)
            return std::unexpected(glyphs.error());
        cff.charstrings = *glyphs;

        const dict_entry fd_array = find_operator(top, 1236);
        if (!fd_array.found)
        {
            auto local = read_local_subrs(table, top);
            if (!local)
                return std::unexpected(local.error());
            cff.local_subrs.push_back(*local);
            return {};
        }

        // CID-keyed: local subroutines per font dict, and which glyph uses
        // which.
        if (fd_array.count < 1 || !valid_offset(fd_array.operands[0], table.size()))
            return std::unexpected("bad CFF FDArray offset");
        const auto dicts = read_index(table, static_cast<std::size_t>(fd_array.operands[0]));
        if (!dicts)
            return std::unexpected(dicts.error());
        for (std::uint32_t i = 0; i < dicts->count && i < 256; ++i)
        {
            auto local = read_local_subrs(table, dicts->item(i));
            if (!local)
                return std::unexpected(local.error());
            cff.local_subrs.push_back(*local);
        }

        const dict_entry fd_select = find_operator(top, 1237);
        if (!fd_select.found || fd_select.count < 1 || !valid_offset(fd_select.operands[0], table.size()))
            return std::unexpected("a CID-keyed CFF font without FDSelect");
        const auto at = static_cast<std::size_t>(fd_select.operands[0]);
        cff.fd_select.assign(face.glyph_count, 0);
        const std::uint8_t format = table.u8(at);
        if (format == 0)
        {
            for (std::uint32_t glyph = 0; glyph < face.glyph_count; ++glyph)
                cff.fd_select[glyph] = table.u8(at + 1 + glyph);
        }
        else if (format == 3)
        {
            const std::size_t ranges = table.fit(at + 3, table.u16(at + 1), 3);
            for (std::size_t r = 0; r < ranges; ++r)
            {
                const std::size_t range = at + 3 + r * 3;
                const std::uint32_t first = table.u16(range);
                // The next range's first glyph, or the sentinel after the last.
                const std::uint32_t last = std::min<std::uint32_t>(table.u16(range + 3), face.glyph_count);
                for (std::uint32_t glyph = first; glyph < last; ++glyph)
                    cff.fd_select[glyph] = table.u8(range + 2);
            }
        }
        else
        {
            return std::unexpected("unsupported CFF FDSelect format");
        }
        return {};
    }

    std::expected<outline, std::string> cff_outline(const face &face, glyph_id glyph)
    {
        const cff_font &cff = face.cff_data;
        if (glyph >= face.glyph_count || glyph >= cff.charstrings.count)
            return std::unexpected("glyph " + std::to_string(glyph) + " is out of range");
        if (cff.local_subrs.empty())
            return std::unexpected("the CFF font is not loaded");

        std::size_t dict = 0;
        if (!cff.fd_select.empty())
            dict = cff.fd_select[glyph];
        if (dict >= cff.local_subrs.size())
            return std::unexpected("glyph " + std::to_string(glyph) + " uses a font dict that does not exist");

        outline result;
        interpreter machine(cff.global_subrs, cff.local_subrs[dict], result);
        if (auto ran = machine.run(cff.charstrings.item(glyph), 0); !ran)
            return std::unexpected(ran.error());
        // A charstring without endchar still ends.
        result.close();
        return result;
    }

} // namespace volt::font::detail
