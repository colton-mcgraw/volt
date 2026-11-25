#include "svg_style.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace volt::image::svg::detail
{

    bool is_space(char c) noexcept
    {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
    }

    std::string_view trim(std::string_view text) noexcept
    {
        while (!text.empty() && is_space(text.front()))
            text.remove_prefix(1);
        while (!text.empty() && is_space(text.back()))
            text.remove_suffix(1);
        return text;
    }

    namespace
    {
        bool is_digit(char c)
        {
            return c >= '0' && c <= '9';
        }

        char lower(char c)
        {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        }

        std::string lowercase(std::string_view text)
        {
            std::string result(text);
            for (char &c : result)
                c = lower(c);
            return result;
        }

        bool equals_ignoring_case(std::string_view a, std::string_view b)
        {
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
                                                      [](char x, char y) { return lower(x) == lower(y); });
        }

        // The length of the number at the start of `text` by SVG's grammar,
        // 0 if there is none.
        std::size_t number_length(std::string_view text)
        {
            std::size_t i = 0;
            if (i < text.size() && (text[i] == '+' || text[i] == '-'))
                ++i;
            const std::size_t digits_start = i;
            while (i < text.size() && is_digit(text[i]))
                ++i;
            bool any = i > digits_start;
            if (i < text.size() && text[i] == '.')
            {
                const std::size_t fraction = ++i;
                while (i < text.size() && is_digit(text[i]))
                    ++i;
                any = any || i > fraction;
            }
            if (!any)
                return 0;
            if (i < text.size() && (text[i] == 'e' || text[i] == 'E'))
            {
                std::size_t j = i + 1;
                if (j < text.size() && (text[j] == '+' || text[j] == '-'))
                    ++j;
                if (j < text.size() && is_digit(text[j]))
                {
                    while (j < text.size() && is_digit(text[j]))
                        ++j;
                    i = j;
                }
            }
            return i;
        }

        bool parse_float(std::string_view text, float &value)
        {
            if (!text.empty() && text.front() == '+')
                text.remove_prefix(1);
            const auto [end, problem] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (problem == std::errc::result_out_of_range)
            {
                // Too large or too small for a float: saturate.
                const bool negative = !text.empty() && text.front() == '-';
                const bool tiny = text.find_first_of("eE") != std::string_view::npos &&
                                  text.find("e-") != std::string_view::npos;
                value = tiny ? 0.0f : (negative ? -3.0e38f : 3.0e38f);
                return true;
            }
            return problem == std::errc{} && end == text.data() + text.size() && std::isfinite(value);
        }
    } // namespace

    // ---- numbers ----------------------------------------------------------------

    void number_reader::skip_spaces() noexcept
    {
        while (m_position < m_text.size() && is_space(m_text[m_position]))
            ++m_position;
    }

    void number_reader::skip_separators() noexcept
    {
        skip_spaces();
        if (m_position < m_text.size() && m_text[m_position] == ',')
        {
            ++m_position;
            skip_spaces();
        }
    }

    bool number_reader::at_end() noexcept
    {
        skip_spaces();
        return m_position >= m_text.size();
    }

    char number_reader::peek() noexcept
    {
        skip_spaces();
        return m_position < m_text.size() ? m_text[m_position] : '\0';
    }

    bool number_reader::number(float &value) noexcept
    {
        const std::size_t saved = m_position;
        skip_separators();
        const std::size_t count = number_length(m_text.substr(m_position));
        if (count == 0 || !parse_float(m_text.substr(m_position, count), value))
        {
            m_position = saved;
            return false;
        }
        m_position += count;
        return true;
    }

    bool number_reader::flag(bool &value) noexcept
    {
        const std::size_t saved = m_position;
        skip_separators();
        if (m_position < m_text.size() && (m_text[m_position] == '0' || m_text[m_position] == '1'))
        {
            value = m_text[m_position++] == '1';
            return true;
        }
        m_position = saved;
        return false;
    }

    std::vector<float> parse_numbers(std::string_view text)
    {
        std::vector<float> result;
        number_reader in(text);
        float value = 0.0f;
        while (in.number(value))
            result.push_back(value);
        return result;
    }

    // ---- lengths ----------------------------------------------------------------

    float length::resolve(float reference) const noexcept
    {
        switch (type)
        {
        case unit::none:
        case unit::px:
            return value;
        case unit::pt:
            return value * 4.0f / 3.0f;
        case unit::pc:
            return value * 16.0f;
        case unit::mm:
            return value * 96.0f / 25.4f;
        case unit::cm:
            return value * 96.0f / 2.54f;
        case unit::in:
            return value * 96.0f;
        case unit::em:
            return value * 16.0f; // no text, so the initial font size
        case unit::ex:
            return value * 8.0f;
        case unit::percent:
            return value / 100.0f * reference;
        }
        return value;
    }

    std::optional<length> parse_length(std::string_view text) noexcept
    {
        text = trim(text);
        const std::size_t count = number_length(text);
        length result;
        if (count == 0 || !parse_float(text.substr(0, count), result.value))
            return std::nullopt;
        const std::string_view unit = trim(text.substr(count));
        struct named
        {
            std::string_view name;
            length::unit type;
        };
        static constexpr named units[] = {
            {"", length::unit::none},  {"px", length::unit::px}, {"pt", length::unit::pt}, {"pc", length::unit::pc},
            {"mm", length::unit::mm},  {"cm", length::unit::cm}, {"in", length::unit::in}, {"em", length::unit::em},
            {"ex", length::unit::ex},  {"%", length::unit::percent},
        };
        for (const named &u : units)
            if (equals_ignoring_case(unit, u.name))
            {
                result.type = u.type;
                return result;
            }
        return std::nullopt;
    }

    // ---- colors -------------------------------------------------------------------

    namespace
    {
        struct named_color
        {
            std::string_view name;
            std::uint32_t rgb;
        };

        // CSS Color 4's named colors, sorted for binary search.
        constexpr named_color named_colors[] = {
            {"aliceblue", 0xF0F8FF},
            {"antiquewhite", 0xFAEBD7},
            {"aqua", 0x00FFFF},
            {"aquamarine", 0x7FFFD4},
            {"azure", 0xF0FFFF},
            {"beige", 0xF5F5DC},
            {"bisque", 0xFFE4C4},
            {"black", 0x000000},
            {"blanchedalmond", 0xFFEBCD},
            {"blue", 0x0000FF},
            {"blueviolet", 0x8A2BE2},
            {"brown", 0xA52A2A},
            {"burlywood", 0xDEB887},
            {"cadetblue", 0x5F9EA0},
            {"chartreuse", 0x7FFF00},
            {"chocolate", 0xD2691E},
            {"coral", 0xFF7F50},
            {"cornflowerblue", 0x6495ED},
            {"cornsilk", 0xFFF8DC},
            {"crimson", 0xDC143C},
            {"cyan", 0x00FFFF},
            {"darkblue", 0x00008B},
            {"darkcyan", 0x008B8B},
            {"darkgoldenrod", 0xB8860B},
            {"darkgray", 0xA9A9A9},
            {"darkgreen", 0x006400},
            {"darkgrey", 0xA9A9A9},
            {"darkkhaki", 0xBDB76B},
            {"darkmagenta", 0x8B008B},
            {"darkolivegreen", 0x556B2F},
            {"darkorange", 0xFF8C00},
            {"darkorchid", 0x9932CC},
            {"darkred", 0x8B0000},
            {"darksalmon", 0xE9967A},
            {"darkseagreen", 0x8FBC8F},
            {"darkslateblue", 0x483D8B},
            {"darkslategray", 0x2F4F4F},
            {"darkslategrey", 0x2F4F4F},
            {"darkturquoise", 0x00CED1},
            {"darkviolet", 0x9400D3},
            {"deeppink", 0xFF1493},
            {"deepskyblue", 0x00BFFF},
            {"dimgray", 0x696969},
            {"dimgrey", 0x696969},
            {"dodgerblue", 0x1E90FF},
            {"firebrick", 0xB22222},
            {"floralwhite", 0xFFFAF0},
            {"forestgreen", 0x228B22},
            {"fuchsia", 0xFF00FF},
            {"gainsboro", 0xDCDCDC},
            {"ghostwhite", 0xF8F8FF},
            {"gold", 0xFFD700},
            {"goldenrod", 0xDAA520},
            {"gray", 0x808080},
            {"green", 0x008000},
            {"greenyellow", 0xADFF2F},
            {"grey", 0x808080},
            {"honeydew", 0xF0FFF0},
            {"hotpink", 0xFF69B4},
            {"indianred", 0xCD5C5C},
            {"indigo", 0x4B0082},
            {"ivory", 0xFFFFF0},
            {"khaki", 0xF0E68C},
            {"lavender", 0xE6E6FA},
            {"lavenderblush", 0xFFF0F5},
            {"lawngreen", 0x7CFC00},
            {"lemonchiffon", 0xFFFACD},
            {"lightblue", 0xADD8E6},
            {"lightcoral", 0xF08080},
            {"lightcyan", 0xE0FFFF},
            {"lightgoldenrodyellow", 0xFAFAD2},
            {"lightgray", 0xD3D3D3},
            {"lightgreen", 0x90EE90},
            {"lightgrey", 0xD3D3D3},
            {"lightpink", 0xFFB6C1},
            {"lightsalmon", 0xFFA07A},
            {"lightseagreen", 0x20B2AA},
            {"lightskyblue", 0x87CEFA},
            {"lightslategray", 0x778899},
            {"lightslategrey", 0x778899},
            {"lightsteelblue", 0xB0C4DE},
            {"lightyellow", 0xFFFFE0},
            {"lime", 0x00FF00},
            {"limegreen", 0x32CD32},
            {"linen", 0xFAF0E6},
            {"magenta", 0xFF00FF},
            {"maroon", 0x800000},
            {"mediumaquamarine", 0x66CDAA},
            {"mediumblue", 0x0000CD},
            {"mediumorchid", 0xBA55D3},
            {"mediumpurple", 0x9370DB},
            {"mediumseagreen", 0x3CB371},
            {"mediumslateblue", 0x7B68EE},
            {"mediumspringgreen", 0x00FA9A},
            {"mediumturquoise", 0x48D1CC},
            {"mediumvioletred", 0xC71585},
            {"midnightblue", 0x191970},
            {"mintcream", 0xF5FFFA},
            {"mistyrose", 0xFFE4E1},
            {"moccasin", 0xFFE4B5},
            {"navajowhite", 0xFFDEAD},
            {"navy", 0x000080},
            {"oldlace", 0xFDF5E6},
            {"olive", 0x808000},
            {"olivedrab", 0x6B8E23},
            {"orange", 0xFFA500},
            {"orangered", 0xFF4500},
            {"orchid", 0xDA70D6},
            {"palegoldenrod", 0xEEE8AA},
            {"palegreen", 0x98FB98},
            {"paleturquoise", 0xAFEEEE},
            {"palevioletred", 0xDB7093},
            {"papayawhip", 0xFFEFD5},
            {"peachpuff", 0xFFDAB9},
            {"peru", 0xCD853F},
            {"pink", 0xFFC0CB},
            {"plum", 0xDDA0DD},
            {"powderblue", 0xB0E0E6},
            {"purple", 0x800080},
            {"rebeccapurple", 0x663399},
            {"red", 0xFF0000},
            {"rosybrown", 0xBC8F8F},
            {"royalblue", 0x4169E1},
            {"saddlebrown", 0x8B4513},
            {"salmon", 0xFA8072},
            {"sandybrown", 0xF4A460},
            {"seagreen", 0x2E8B57},
            {"seashell", 0xFFF5EE},
            {"sienna", 0xA0522D},
            {"silver", 0xC0C0C0},
            {"skyblue", 0x87CEEB},
            {"slateblue", 0x6A5ACD},
            {"slategray", 0x708090},
            {"slategrey", 0x708090},
            {"snow", 0xFFFAFA},
            {"springgreen", 0x00FF7F},
            {"steelblue", 0x4682B4},
            {"tan", 0xD2B48C},
            {"teal", 0x008080},
            {"thistle", 0xD8BFD8},
            {"tomato", 0xFF6347},
            {"turquoise", 0x40E0D0},
            {"violet", 0xEE82EE},
            {"wheat", 0xF5DEB3},
            {"white", 0xFFFFFF},
            {"whitesmoke", 0xF5F5F5},
            {"yellow", 0xFFFF00},
            {"yellowgreen", 0x9ACD32},
        };

        math::vec4f from_rgb(std::uint32_t rgb, float alpha = 1.0f)
        {
            return {static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                    static_cast<float>(rgb & 0xFF) / 255.0f, alpha};
        }

        int hex_digit(char c)
        {
            if (c >= '0' && c <= '9')
                return c - '0';
            c = lower(c);
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            return -1;
        }

        // The arguments of rgb(), rgba(), hsl(), hsla(): numbers or
        // percentages, separated by commas, spaces or a slash before alpha.
        bool function_arguments(std::string_view inside, float (&values)[4], bool (&percent)[4], int &count)
        {
            count = 0;
            std::size_t i = 0;
            while (count < 4)
            {
                while (i < inside.size() && (is_space(inside[i]) || inside[i] == ',' || inside[i] == '/'))
                    ++i;
                if (i >= inside.size())
                    break;
                const std::size_t n = number_length(inside.substr(i));
                if (n == 0 || !parse_float(inside.substr(i, n), values[count]))
                    return false;
                i += n;
                percent[count] = i < inside.size() && inside[i] == '%';
                if (percent[count])
                    ++i;
                // Units on hue are ignored ("120deg").
                while (i < inside.size() && ((inside[i] >= 'a' && inside[i] <= 'z') || (inside[i] >= 'A' && inside[i] <= 'Z')))
                    ++i;
                ++count;
            }
            return count >= 3;
        }

        float hue_to_rgb(float p, float q, float t)
        {
            if (t < 0.0f)
                t += 1.0f;
            if (t > 1.0f)
                t -= 1.0f;
            if (t < 1.0f / 6.0f)
                return p + (q - p) * 6.0f * t;
            if (t < 0.5f)
                return q;
            if (t < 2.0f / 3.0f)
                return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
            return p;
        }
    } // namespace

    std::optional<math::vec4f> parse_color(std::string_view text) noexcept
    {
        text = trim(text);
        if (text.empty())
            return std::nullopt;

        if (text.front() == '#')
        {
            const std::string_view hex = text.substr(1);
            int digits[8] = {};
            if (hex.size() != 3 && hex.size() != 4 && hex.size() != 6 && hex.size() != 8)
                return std::nullopt;
            for (std::size_t i = 0; i < hex.size(); ++i)
                if ((digits[i] = hex_digit(hex[i])) < 0)
                    return std::nullopt;
            const auto channel = [&](std::size_t index) -> float {
                if (hex.size() <= 4)
                    return static_cast<float>(digits[index] * 17) / 255.0f;
                return static_cast<float>(digits[2 * index] * 16 + digits[2 * index + 1]) / 255.0f;
            };
            const bool alpha = hex.size() == 4 || hex.size() == 8;
            return math::vec4f{channel(0), channel(1), channel(2), alpha ? channel(3) : 1.0f};
        }

        const std::size_t open = text.find('(');
        if (open != std::string_view::npos)
        {
            if (text.back() != ')')
                return std::nullopt;
            const std::string name = lowercase(trim(text.substr(0, open)));
            const std::string_view inside = text.substr(open + 1, text.size() - open - 2);
            float v[4] = {};
            bool percent[4] = {};
            int count = 0;
            if (!function_arguments(inside, v, percent, count))
                return std::nullopt;
            float alpha = 1.0f;
            if (count == 4)
                alpha = std::clamp(percent[3] ? v[3] / 100.0f : v[3], 0.0f, 1.0f);

            if (name == "rgb" || name == "rgba")
            {
                const auto channel = [&](int i) {
                    return std::clamp(percent[i] ? v[i] / 100.0f : v[i] / 255.0f, 0.0f, 1.0f);
                };
                return math::vec4f{channel(0), channel(1), channel(2), alpha};
            }
            if (name == "hsl" || name == "hsla")
            {
                float h = std::fmod(v[0], 360.0f) / 360.0f;
                if (h < 0.0f)
                    h += 1.0f;
                const float s = std::clamp(v[1] / 100.0f, 0.0f, 1.0f);
                const float l = std::clamp(v[2] / 100.0f, 0.0f, 1.0f);
                const float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
                const float p = 2.0f * l - q;
                return math::vec4f{hue_to_rgb(p, q, h + 1.0f / 3.0f), hue_to_rgb(p, q, h), hue_to_rgb(p, q, h - 1.0f / 3.0f),
                                   alpha};
            }
            return std::nullopt;
        }

        const std::string name = lowercase(text);
        if (name == "transparent")
            return math::vec4f{0, 0, 0, 0};
        const auto found = std::lower_bound(std::begin(named_colors), std::end(named_colors), name,
                                            [](const named_color &c, const std::string &key) { return c.name < key; });
        if (found != std::end(named_colors) && found->name == name)
            return from_rgb(found->rgb);
        return std::nullopt;
    }

    // ---- transforms ---------------------------------------------------------------

    math::mat3f translation(float x, float y) noexcept
    {
        return math::mat3f::from_rows({{1, 0, x}, {0, 1, y}, {0, 0, 1}});
    }

    math::mat3f scaling(float x, float y) noexcept
    {
        return math::mat3f::from_rows({{x, 0, 0}, {0, y, 0}, {0, 0, 1}});
    }

    std::optional<math::mat3f> parse_transform(std::string_view text)
    {
        math::mat3f result = math::mat3f::identity();
        std::size_t i = 0;
        for (;;)
        {
            while (i < text.size() && (is_space(text[i]) || text[i] == ','))
                ++i;
            if (i >= text.size())
                return result;
            const std::size_t name_start = i;
            while (i < text.size() && ((text[i] >= 'a' && text[i] <= 'z') || (text[i] >= 'A' && text[i] <= 'Z')))
                ++i;
            const std::string_view name = text.substr(name_start, i - name_start);
            while (i < text.size() && is_space(text[i]))
                ++i;
            if (i >= text.size() || text[i] != '(')
                return std::nullopt;
            const std::size_t close = text.find(')', i);
            if (close == std::string_view::npos)
                return std::nullopt;
            const std::vector<float> v = parse_numbers(text.substr(i + 1, close - i - 1));
            i = close + 1;

            const float degrees = std::numbers::pi_v<float> / 180.0f;
            math::mat3f step = math::mat3f::identity();
            if (name == "matrix" && v.size() == 6)
                step = math::mat3f::from_rows({{v[0], v[2], v[4]}, {v[1], v[3], v[5]}, {0, 0, 1}});
            else if (name == "translate" && (v.size() == 1 || v.size() == 2))
                step = translation(v[0], v.size() == 2 ? v[1] : 0.0f);
            else if (name == "scale" && (v.size() == 1 || v.size() == 2))
                step = scaling(v[0], v.size() == 2 ? v[1] : v[0]);
            else if (name == "rotate" && (v.size() == 1 || v.size() == 3))
            {
                const float c = std::cos(v[0] * degrees);
                const float s = std::sin(v[0] * degrees);
                step = math::mat3f::from_rows({{c, -s, 0}, {s, c, 0}, {0, 0, 1}});
                if (v.size() == 3)
                    step = translation(v[1], v[2]) * step * translation(-v[1], -v[2]);
            }
            else if (name == "skewX" && v.size() == 1)
                step = math::mat3f::from_rows({{1, std::tan(v[0] * degrees), 0}, {0, 1, 0}, {0, 0, 1}});
            else if (name == "skewY" && v.size() == 1)
                step = math::mat3f::from_rows({{1, 0, 0}, {std::tan(v[0] * degrees), 1, 0}, {0, 0, 1}});
            else
                return std::nullopt;
            result = result * step;
        }
    }

    // ---- CSS ------------------------------------------------------------------------

    namespace
    {
        std::string strip_comments(std::string_view text)
        {
            std::string result;
            std::size_t i = 0;
            while (i < text.size())
            {
                const std::size_t start = text.find("/*", i);
                if (start == std::string_view::npos)
                {
                    result.append(text.substr(i));
                    break;
                }
                result.append(text.substr(i, start - i));
                const std::size_t end = text.find("*/", start + 2);
                if (end == std::string_view::npos)
                    break;
                i = end + 2;
            }
            return result;
        }
    } // namespace

    declarations parse_declarations(std::string_view text)
    {
        const std::string clean = strip_comments(text);
        const std::string_view all(clean);
        declarations result;
        std::size_t i = 0;
        while (i < all.size())
        {
            // To the next semicolon outside parentheses and quotes.
            std::size_t end = i;
            int depth = 0;
            char quote = 0;
            for (; end < all.size(); ++end)
            {
                const char c = all[end];
                if (quote != 0)
                {
                    if (c == quote)
                        quote = 0;
                }
                else if (c == '"' || c == '\'')
                    quote = c;
                else if (c == '(')
                    ++depth;
                else if (c == ')')
                    depth = std::max(0, depth - 1);
                else if (c == ';' && depth == 0)
                    break;
            }
            const std::string_view declaration = all.substr(i, end - i);
            i = end + 1;
            const std::size_t colon = declaration.find(':');
            if (colon == std::string_view::npos)
                continue;
            const std::string name = lowercase(trim(declaration.substr(0, colon)));
            std::string_view value = trim(declaration.substr(colon + 1));
            const std::size_t important = value.find("!important");
            if (important != std::string_view::npos)
                value = trim(value.substr(0, important));
            if (!name.empty())
                result.emplace_back(name, std::string(value));
        }
        return result;
    }

    void style_sheet::add(std::string_view text)
    {
        const std::string clean = strip_comments(text);
        const std::string_view all(clean);
        std::size_t i = 0;
        while (i < all.size())
        {
            while (i < all.size() && is_space(all[i]))
                ++i;
            if (i >= all.size())
                break;

            if (all[i] == '@')
            {
                // An at-rule: to its semicolon, or past its block.
                const std::size_t semicolon = all.find(';', i);
                const std::size_t brace = all.find('{', i);
                if (brace == std::string_view::npos || (semicolon != std::string_view::npos && semicolon < brace))
                {
                    i = semicolon == std::string_view::npos ? all.size() : semicolon + 1;
                    continue;
                }
                int depth = 0;
                std::size_t j = brace;
                for (; j < all.size(); ++j)
                {
                    if (all[j] == '{')
                        ++depth;
                    else if (all[j] == '}' && --depth == 0)
                        break;
                }
                i = j + 1;
                continue;
            }

            const std::size_t open = all.find('{', i);
            if (open == std::string_view::npos)
                break;
            const std::size_t close = all.find('}', open);
            const std::string_view selectors = all.substr(i, open - i);
            const std::string_view body =
                all.substr(open + 1, (close == std::string_view::npos ? all.size() : close) - open - 1);
            i = close == std::string_view::npos ? all.size() : close + 1;

            const declarations parsed = parse_declarations(body);
            std::size_t start = 0;
            while (start <= selectors.size())
            {
                std::size_t comma = selectors.find(',', start);
                if (comma == std::string_view::npos)
                    comma = selectors.size();
                const std::string_view text_selector = trim(selectors.substr(start, comma - start));
                start = comma + 1;
                if (text_selector.empty())
                    continue;

                // A compound of type, .class and #id; anything else (combinators,
                // attributes, pseudo-classes) is not supported and the
                // selector is dropped.
                selector parsed_selector;
                bool supported = true;
                std::size_t k = 0;
                const auto read_identifier = [&]() {
                    const std::size_t from = k;
                    while (k < text_selector.size() &&
                           (std::isalnum(static_cast<unsigned char>(text_selector[k])) || text_selector[k] == '-' ||
                            text_selector[k] == '_' || static_cast<unsigned char>(text_selector[k]) >= 0x80))
                        ++k;
                    return std::string(text_selector.substr(from, k - from));
                };
                if (k < text_selector.size() && text_selector[k] == '*')
                    ++k;
                else if (k < text_selector.size() && text_selector[k] != '.' && text_selector[k] != '#')
                {
                    parsed_selector.tag = read_identifier();
                    parsed_selector.specificity += 1;
                    if (parsed_selector.tag.empty())
                        supported = false;
                }
                while (supported && k < text_selector.size())
                {
                    const char kind = text_selector[k++];
                    const std::string name = read_identifier();
                    if (name.empty())
                        supported = false;
                    else if (kind == '.')
                    {
                        parsed_selector.classes.push_back(name);
                        parsed_selector.specificity += 100;
                    }
                    else if (kind == '#')
                    {
                        parsed_selector.id = name;
                        parsed_selector.specificity += 10000;
                    }
                    else
                        supported = false;
                }
                if (supported)
                    m_rules.push_back({std::move(parsed_selector), parsed, m_rules.size()});
            }
        }
    }

    declarations style_sheet::match(const xml::element &element) const
    {
        std::vector<const rule *> matched;
        const auto id = element.find("id");
        const auto classes = element.find("class");
        for (const rule &r : m_rules)
        {
            const selector &s = r.match;
            if (!s.tag.empty() && s.tag != element.local_name())
                continue;
            if (!s.id.empty() && (!id || *id != s.id))
                continue;
            bool all_classes = true;
            for (const std::string &wanted : s.classes)
            {
                bool found = false;
                if (classes)
                {
                    std::string_view list = *classes;
                    while (!list.empty() && !found)
                    {
                        while (!list.empty() && is_space(list.front()))
                            list.remove_prefix(1);
                        std::size_t end = 0;
                        while (end < list.size() && !is_space(list[end]))
                            ++end;
                        found = list.substr(0, end) == wanted;
                        list.remove_prefix(end);
                    }
                }
                if (!found)
                {
                    all_classes = false;
                    break;
                }
            }
            if (all_classes)
                matched.push_back(&r);
        }
        std::stable_sort(matched.begin(), matched.end(), [](const rule *a, const rule *b) {
            return a->match.specificity < b->match.specificity;
        });

        declarations result;
        for (const rule *r : matched)
            result.insert(result.end(), r->body.begin(), r->body.end());
        return result;
    }

} // namespace volt::image::svg::detail
