#include <json/json.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <fstream>
#include <system_error>

namespace volt::json
{

    namespace
    {
        const value &null_value() noexcept
        {
            static const value null;
            return null;
        }

        // 2^53: beyond it not every integer is a double.
        constexpr double max_exact_integer = 9007199254740992.0;

        // ---- UTF-8 -----------------------------------------------------------

        void append_utf8(std::string &out, std::uint32_t code)
        {
            if (code < 0x80)
                out += static_cast<char>(code);
            else if (code < 0x800)
            {
                out += static_cast<char>(0xC0 | (code >> 6));
                out += static_cast<char>(0x80 | (code & 0x3F));
            }
            else if (code < 0x10000)
            {
                out += static_cast<char>(0xE0 | (code >> 12));
                out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (code & 0x3F));
            }
            else
            {
                out += static_cast<char>(0xF0 | (code >> 18));
                out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (code & 0x3F));
            }
        }

        // Decodes one well-formed UTF-8 sequence at text[at]: the shortest
        // form, no surrogates, nothing above U+10FFFF (RFC 3629). Returns its
        // length and sets code, or returns 0 for anything else.
        std::size_t decode_utf8(std::string_view text, std::size_t at, std::uint32_t &code) noexcept
        {
            const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
            const unsigned char lead = byte(at);
            std::size_t length = 0;
            std::uint32_t minimum = 0;
            if (lead < 0x80)
            {
                code = lead;
                return 1;
            }
            if ((lead & 0xE0) == 0xC0)
            {
                length = 2;
                code = lead & 0x1Fu;
                minimum = 0x80;
            }
            else if ((lead & 0xF0) == 0xE0)
            {
                length = 3;
                code = lead & 0x0Fu;
                minimum = 0x800;
            }
            else if ((lead & 0xF8) == 0xF0)
            {
                length = 4;
                code = lead & 0x07u;
                minimum = 0x10000;
            }
            else
                return 0;
            if (at + length > text.size())
                return 0;
            for (std::size_t i = 1; i < length; ++i)
            {
                const unsigned char next = byte(at + i);
                if ((next & 0xC0) != 0x80)
                    return 0;
                code = (code << 6) | (next & 0x3Fu);
            }
            if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
                return 0;
            return length;
        }

        // ---- reading ---------------------------------------------------------

        bool is_space(char c) noexcept
        {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r';
        }

        bool is_digit(char c) noexcept
        {
            return c >= '0' && c <= '9';
        }

        struct failure
        {
            std::size_t offset;
            std::string reason;
        };

        class parser
        {
        public:
            parser(std::string_view text, const parse_options &options) : m_text(text), m_options(options) {}

            std::expected<value, failure> run()
            {
                if (m_text.starts_with("\xEF\xBB\xBF"))
                    m_at = 3;
                skip_space();
                if (m_at == m_text.size())
                    return fail("the text is empty, a value was expected");
                value result;
                if (auto ok = parse_value(result, 0); !ok)
                    return std::unexpected(std::move(ok.error()));
                skip_space();
                if (m_at != m_text.size())
                    return fail("unexpected text after the value");
                return result;
            }

        private:
            using step = std::expected<void, failure>;

            std::unexpected<failure> fail(std::string reason) const
            {
                return std::unexpected(failure{m_at, std::move(reason)});
            }

            std::unexpected<failure> fail_at(std::size_t offset, std::string reason) const
            {
                return std::unexpected(failure{offset, std::move(reason)});
            }

            // What is at the current position, for messages.
            std::string found() const
            {
                if (m_at >= m_text.size())
                    return "the end of the text";
                const char c = m_text[m_at];
                if (static_cast<unsigned char>(c) < 0x20)
                    return std::format("a control character (0x{:02X})", static_cast<unsigned>(c));
                if (static_cast<unsigned char>(c) >= 0x80)
                    return "a non-ASCII character";
                return std::format("'{}'", c);
            }

            void skip_space() noexcept
            {
                while (m_at < m_text.size() && is_space(m_text[m_at]))
                    ++m_at;
            }

            bool consume(std::string_view word) noexcept
            {
                if (m_text.substr(m_at).starts_with(word))
                {
                    m_at += word.size();
                    return true;
                }
                return false;
            }

            step parse_value(value &out, std::size_t depth)
            {
                if (m_at >= m_text.size())
                    return fail("a value was expected, the text ends");
                switch (m_text[m_at])
                {
                case '{':
                    return parse_object(out, depth + 1);
                case '[':
                    return parse_array(out, depth + 1);
                case '"':
                {
                    std::string s;
                    if (auto ok = parse_string(s); !ok)
                        return ok;
                    out = value(std::move(s));
                    return {};
                }
                case 't':
                    if (!consume("true"))
                        break;
                    out = value(true);
                    return {};
                case 'f':
                    if (!consume("false"))
                        break;
                    out = value(false);
                    return {};
                case 'n':
                    if (!consume("null"))
                        break;
                    out = value();
                    return {};
                default:
                    if (m_text[m_at] == '-' || is_digit(m_text[m_at]))
                        return parse_number(out);
                    break;
                }
                return fail("a value was expected, found " + found());
            }

            step enter(std::size_t depth) const
            {
                if (depth > m_options.max_depth)
                    return fail(std::format("nested deeper than the limit of {}", m_options.max_depth));
                return {};
            }

            step parse_object(value &out, std::size_t depth)
            {
                if (auto ok = enter(depth); !ok)
                    return ok;
                ++m_at; // {
                object result;
                skip_space();
                if (m_at < m_text.size() && m_text[m_at] == '}')
                {
                    ++m_at;
                    out = value(std::move(result));
                    return {};
                }
                while (true)
                {
                    skip_space();
                    if (m_at >= m_text.size() || m_text[m_at] != '"')
                        return fail("a key (a string in double quotes) was expected, found " + found());
                    const std::size_t key_at = m_at;
                    std::string key;
                    if (auto ok = parse_string(key); !ok)
                        return ok;
                    skip_space();
                    if (m_at >= m_text.size() || m_text[m_at] != ':')
                        return fail("':' was expected after the key, found " + found());
                    ++m_at;
                    skip_space();
                    value element;
                    if (auto ok = parse_value(element, depth); !ok)
                        return ok;
                    if (value *existing = result.find(key))
                    {
                        if (!m_options.allow_duplicate_keys)
                            return fail_at(key_at, std::format("the key \"{}\" appears twice in this object", key));
                        *existing = std::move(element);
                    }
                    else
                        result.set(key, std::move(element));
                    skip_space();
                    if (m_at < m_text.size() && m_text[m_at] == ',')
                    {
                        ++m_at;
                        skip_space();
                        if (m_at < m_text.size() && m_text[m_at] == '}')
                            return fail("a trailing comma is not allowed before '}'");
                        continue;
                    }
                    if (m_at < m_text.size() && m_text[m_at] == '}')
                    {
                        ++m_at;
                        out = value(std::move(result));
                        return {};
                    }
                    return fail("',' or '}' was expected, found " + found());
                }
            }

            step parse_array(value &out, std::size_t depth)
            {
                if (auto ok = enter(depth); !ok)
                    return ok;
                ++m_at; // [
                array result;
                skip_space();
                if (m_at < m_text.size() && m_text[m_at] == ']')
                {
                    ++m_at;
                    out = value(std::move(result));
                    return {};
                }
                while (true)
                {
                    skip_space();
                    value element;
                    if (auto ok = parse_value(element, depth); !ok)
                        return ok;
                    result.push_back(std::move(element));
                    skip_space();
                    if (m_at < m_text.size() && m_text[m_at] == ',')
                    {
                        ++m_at;
                        skip_space();
                        if (m_at < m_text.size() && m_text[m_at] == ']')
                            return fail("a trailing comma is not allowed before ']'");
                        continue;
                    }
                    if (m_at < m_text.size() && m_text[m_at] == ']')
                    {
                        ++m_at;
                        out = value(std::move(result));
                        return {};
                    }
                    return fail("',' or ']' was expected, found " + found());
                }
            }

            // Four hex digits after \u.
            std::expected<std::uint32_t, failure> hex4()
            {
                if (m_at + 4 > m_text.size())
                    return fail("\\u needs four hex digits");
                std::uint32_t code = 0;
                for (std::size_t i = 0; i < 4; ++i)
                {
                    const char c = m_text[m_at + i];
                    std::uint32_t digit = 0;
                    if (c >= '0' && c <= '9')
                        digit = static_cast<std::uint32_t>(c - '0');
                    else if (c >= 'a' && c <= 'f')
                        digit = static_cast<std::uint32_t>(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F')
                        digit = static_cast<std::uint32_t>(c - 'A' + 10);
                    else
                        return fail_at(m_at + i, "\\u needs four hex digits");
                    code = code * 16 + digit;
                }
                m_at += 4;
                return code;
            }

            step parse_string(std::string &out)
            {
                const std::size_t open = m_at;
                ++m_at; // "
                while (true)
                {
                    // The plain run up to the next quote, backslash or
                    // non-ASCII byte is copied in one go.
                    const std::size_t start = m_at;
                    while (m_at < m_text.size())
                    {
                        const auto c = static_cast<unsigned char>(m_text[m_at]);
                        if (c == '"' || c == '\\' || c < 0x20 || c >= 0x80)
                            break;
                        ++m_at;
                    }
                    out.append(m_text.substr(start, m_at - start));
                    if (m_at >= m_text.size())
                        return fail_at(open, "the string is not closed (a '\"' is missing)");

                    const auto c = static_cast<unsigned char>(m_text[m_at]);
                    if (c == '"')
                    {
                        ++m_at;
                        return {};
                    }
                    if (c < 0x20)
                        return fail(std::format("a control character (0x{:02X}) must be escaped in a string",
                                                static_cast<unsigned>(c)));
                    if (c >= 0x80)
                    {
                        std::uint32_t code = 0;
                        const std::size_t length = decode_utf8(m_text, m_at, code);
                        if (length == 0)
                            return fail("invalid UTF-8 in a string");
                        out.append(m_text.substr(m_at, length));
                        m_at += length;
                        continue;
                    }

                    // A backslash.
                    const std::size_t escape_at = m_at;
                    ++m_at;
                    if (m_at >= m_text.size())
                        return fail_at(open, "the string is not closed (a '\"' is missing)");
                    const char e = m_text[m_at++];
                    switch (e)
                    {
                    case '"':
                        out += '"';
                        break;
                    case '\\':
                        out += '\\';
                        break;
                    case '/':
                        out += '/';
                        break;
                    case 'b':
                        out += '\b';
                        break;
                    case 'f':
                        out += '\f';
                        break;
                    case 'n':
                        out += '\n';
                        break;
                    case 'r':
                        out += '\r';
                        break;
                    case 't':
                        out += '\t';
                        break;
                    case 'u':
                    {
                        auto code = hex4();
                        if (!code)
                            return std::unexpected(std::move(code.error()));
                        std::uint32_t c32 = *code;
                        if (c32 >= 0xDC00 && c32 <= 0xDFFF)
                            return fail_at(escape_at, "a low surrogate (\\uDC00 to \\uDFFF) without a high one before it");
                        if (c32 >= 0xD800 && c32 <= 0xDBFF)
                        {
                            if (!m_text.substr(m_at).starts_with("\\u"))
                                return fail_at(escape_at, "a high surrogate (\\uD800 to \\uDBFF) without a low one after it");
                            m_at += 2;
                            auto low = hex4();
                            if (!low)
                                return std::unexpected(std::move(low.error()));
                            if (*low < 0xDC00 || *low > 0xDFFF)
                                return fail_at(escape_at, "a high surrogate (\\uD800 to \\uDBFF) without a low one after it");
                            c32 = 0x10000 + ((c32 - 0xD800) << 10) + (*low - 0xDC00);
                        }
                        append_utf8(out, c32);
                        break;
                    }
                    default:
                        return fail_at(escape_at, std::format("unknown escape '\\{}'", e));
                    }
                }
            }

            step parse_number(value &out)
            {
                const std::size_t start = m_at;
                if (m_text[m_at] == '-')
                    ++m_at;
                if (m_at >= m_text.size() || !is_digit(m_text[m_at]))
                    return fail("a digit was expected after '-'");

                // Where the first significant digit sits, as a power of ten,
                // to tell an overflow from an underflow below.
                long long decade = 0;
                if (m_text[m_at] == '0')
                {
                    ++m_at;
                    if (m_at < m_text.size() && is_digit(m_text[m_at]))
                        return fail("a number cannot have a leading zero");
                    decade = -1;
                }
                else
                {
                    while (m_at < m_text.size() && is_digit(m_text[m_at]))
                    {
                        ++m_at;
                        ++decade;
                    }
                    --decade;
                }
                if (m_at < m_text.size() && m_text[m_at] == '.')
                {
                    ++m_at;
                    if (m_at >= m_text.size() || !is_digit(m_text[m_at]))
                        return fail("a digit was expected after '.'");
                    bool significant = decade >= 0;
                    while (m_at < m_text.size() && is_digit(m_text[m_at]))
                    {
                        if (!significant && m_text[m_at] == '0')
                            --decade;
                        else
                            significant = true;
                        ++m_at;
                    }
                }
                long long exponent = 0;
                if (m_at < m_text.size() && (m_text[m_at] == 'e' || m_text[m_at] == 'E'))
                {
                    ++m_at;
                    bool negative = false;
                    if (m_at < m_text.size() && (m_text[m_at] == '+' || m_text[m_at] == '-'))
                        negative = m_text[m_at++] == '-';
                    if (m_at >= m_text.size() || !is_digit(m_text[m_at]))
                        return fail("a digit was expected in the exponent");
                    while (m_at < m_text.size() && is_digit(m_text[m_at]))
                    {
                        if (exponent < 100000)
                            exponent = exponent * 10 + (m_text[m_at] - '0');
                        ++m_at;
                    }
                    if (negative)
                        exponent = -exponent;
                }

                const std::string_view text = m_text.substr(start, m_at - start);
                double number = 0.0;
                const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
                if (error == std::errc::result_out_of_range)
                {
                    // Too small to be anything but zero is zero, as every
                    // other JSON reader has it; too large is an error, since
                    // there is no infinity in JSON to stand for it.
                    if (decade + exponent < 0)
                        number = text.front() == '-' ? -0.0 : 0.0;
                    else
                        return fail_at(start, "the number is too large for a double");
                }
                else if (error != std::errc{} || end != text.data() + text.size())
                    return fail_at(start, "not a valid number");
                out = value(number);
                return {};
            }

            std::string_view m_text;
            const parse_options &m_options;
            std::size_t m_at = 0;
        };

        // ---- writing ---------------------------------------------------------

        void write_number(std::string &out, double number)
        {
            if (!std::isfinite(number))
            {
                out += "null";
                return;
            }
            char buffer[32];
            std::to_chars_result result{};
            if (number == std::trunc(number) && std::abs(number) <= max_exact_integer)
                result = std::to_chars(buffer, buffer + sizeof buffer, static_cast<long long>(number));
            else
                result = std::to_chars(buffer, buffer + sizeof buffer, number);
            out.append(buffer, result.ptr);
        }

        void write_escape(std::string &out, std::uint32_t unit)
        {
            constexpr char hex[] = "0123456789abcdef";
            out += "\\u";
            out += hex[(unit >> 12) & 0xF];
            out += hex[(unit >> 8) & 0xF];
            out += hex[(unit >> 4) & 0xF];
            out += hex[unit & 0xF];
        }

        void write_string(std::string &out, std::string_view text, bool ascii)
        {
            out += '"';
            std::size_t at = 0;
            while (at < text.size())
            {
                const auto c = static_cast<unsigned char>(text[at]);
                if (c >= 0x80)
                {
                    std::uint32_t code = 0;
                    std::size_t length = decode_utf8(text, at, code);
                    if (length == 0)
                    {
                        code = 0xFFFD;
                        length = 1;
                    }
                    if (ascii)
                    {
                        if (code >= 0x10000)
                        {
                            write_escape(out, 0xD800 + ((code - 0x10000) >> 10));
                            write_escape(out, 0xDC00 + ((code - 0x10000) & 0x3FF));
                        }
                        else
                            write_escape(out, code);
                    }
                    else if (code == 0xFFFD && length == 1)
                        append_utf8(out, code);
                    else
                        out.append(text.substr(at, length));
                    at += length;
                    continue;
                }
                switch (c)
                {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\b':
                    out += "\\b";
                    break;
                case '\f':
                    out += "\\f";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    if (c < 0x20)
                        write_escape(out, c);
                    else
                        out += static_cast<char>(c);
                }
                ++at;
            }
            out += '"';
        }

        // Characters on the current line, in code points.
        std::size_t current_column(const std::string &out) noexcept
        {
            const std::size_t newline = out.rfind('\n');
            const std::size_t start = newline == std::string::npos ? 0 : newline + 1;
            std::size_t column = 0;
            for (std::size_t i = start; i < out.size(); ++i)
                column += (static_cast<unsigned char>(out[i]) & 0xC0) != 0x80;
            return column;
        }

        std::size_t width_of(std::string_view text) noexcept
        {
            std::size_t width = 0;
            for (const char c : text)
                width += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
            return width;
        }

        bool is_flat(const value &v) noexcept
        {
            if (const array *a = v.as_array())
            {
                for (const value &element : *a)
                    if (element.size() != 0)
                        return false;
                return true;
            }
            if (const object *o = v.as_object())
            {
                for (const member &m : *o)
                    if (m.value.size() != 0)
                        return false;
                return true;
            }
            return true;
        }

        class writer
        {
        public:
            explicit writer(const write_options &options) : m_options(options) {}

            std::string run(const value &v)
            {
                write(v, 0);
                if (m_options.pretty && m_options.final_newline)
                    m_out += '\n';
                return std::move(m_out);
            }

        private:
            void newline(std::size_t level)
            {
                m_out += '\n';
                m_out.append(level * static_cast<std::size_t>(m_options.indent < 0 ? 0 : m_options.indent), ' ');
            }

            // A flat container on one line: "[1, 2]", {"a": 1, "b": 2}.
            std::string one_line(const value &v) const
            {
                std::string line;
                if (const array *a = v.as_array())
                {
                    line += '[';
                    for (std::size_t i = 0; i < a->size(); ++i)
                    {
                        if (i != 0)
                            line += ", ";
                        scalar(line, (*a)[i]);
                    }
                    line += ']';
                }
                else if (const object *o = v.as_object())
                {
                    line += '{';
                    bool first = true;
                    for (const member &m : *o)
                    {
                        if (!first)
                            line += ", ";
                        first = false;
                        write_string(line, m.key, m_options.ascii);
                        line += ": ";
                        scalar(line, m.value);
                    }
                    line += '}';
                }
                return line;
            }

            // A scalar, or an empty container.
            void scalar(std::string &out, const value &v) const
            {
                switch (v.type())
                {
                case kind::null:
                    out += "null";
                    break;
                case kind::boolean:
                    out += *v.as_bool() ? "true" : "false";
                    break;
                case kind::number:
                    write_number(out, *v.as_number());
                    break;
                case kind::string:
                    write_string(out, *v.as_string(), m_options.ascii);
                    break;
                case kind::array:
                    out += "[]";
                    break;
                case kind::object:
                    out += "{}";
                    break;
                }
            }

            void write(const value &v, std::size_t level)
            {
                if (v.size() == 0)
                {
                    scalar(m_out, v);
                    return;
                }
                if (!m_options.pretty)
                {
                    compact(v);
                    return;
                }
                if (m_options.line_width != 0 && is_flat(v))
                {
                    // +1 for the comma that may follow.
                    const std::string line = one_line(v);
                    if (current_column(m_out) + width_of(line) + 1 <= m_options.line_width)
                    {
                        m_out += line;
                        return;
                    }
                }
                if (const array *a = v.as_array())
                {
                    m_out += '[';
                    for (std::size_t i = 0; i < a->size(); ++i)
                    {
                        newline(level + 1);
                        write((*a)[i], level + 1);
                        if (i + 1 != a->size())
                            m_out += ',';
                    }
                    newline(level);
                    m_out += ']';
                }
                else if (const object *o = v.as_object())
                {
                    m_out += '{';
                    std::size_t i = 0;
                    for (const member &m : *o)
                    {
                        newline(level + 1);
                        write_string(m_out, m.key, m_options.ascii);
                        m_out += ": ";
                        write(m.value, level + 1);
                        if (++i != o->size())
                            m_out += ',';
                    }
                    newline(level);
                    m_out += '}';
                }
            }

            void compact(const value &v)
            {
                if (const array *a = v.as_array())
                {
                    m_out += '[';
                    for (std::size_t i = 0; i < a->size(); ++i)
                    {
                        if (i != 0)
                            m_out += ',';
                        compact((*a)[i]);
                    }
                    m_out += ']';
                }
                else if (const object *o = v.as_object())
                {
                    m_out += '{';
                    bool first = true;
                    for (const member &m : *o)
                    {
                        if (!first)
                            m_out += ',';
                        first = false;
                        write_string(m_out, m.key, m_options.ascii);
                        m_out += ':';
                        compact(m.value);
                    }
                    m_out += '}';
                }
                else
                    scalar(m_out, v);
            }

            const write_options &m_options;
            std::string m_out;
        };

        std::string shown_name(const std::filesystem::path &path)
        {
            const std::u8string name = path.filename().u8string();
            return std::string(name.begin(), name.end());
        }
    } // namespace

    std::string_view describe(kind k) noexcept
    {
        switch (k)
        {
        case kind::null:
            return "null";
        case kind::boolean:
            return "a boolean";
        case kind::number:
            return "a number";
        case kind::string:
            return "a string";
        case kind::array:
            return "an array";
        case kind::object:
            return "an object";
        }
        return "unknown";
    }

    // ---- object ----------------------------------------------------------------

    object::object(std::initializer_list<member> members)
    {
        m_members.reserve(members.size());
        for (const member &m : members)
            set(m.key, m.value);
    }

    value &object::set(std::string_view key, value v)
    {
        if (value *existing = find(key))
        {
            *existing = std::move(v);
            return *existing;
        }
        m_members.push_back(member{std::string(key), std::move(v)});
        return m_members.back().value;
    }

    value &object::operator[](std::string_view key)
    {
        if (value *existing = find(key))
            return *existing;
        m_members.push_back(member{std::string(key), value()});
        return m_members.back().value;
    }

    bool object::erase(std::string_view key)
    {
        for (auto it = m_members.begin(); it != m_members.end(); ++it)
        {
            if (it->key == key)
            {
                m_members.erase(it);
                return true;
            }
        }
        return false;
    }

    bool object::operator==(const object &other) const
    {
        if (m_members.size() != other.m_members.size())
            return false;
        // The common case, the same order, in one pass.
        if (m_members == other.m_members)
            return true;
        for (const member &m : m_members)
        {
            const value *theirs = other.find(m.key);
            if (theirs == nullptr || !(*theirs == m.value))
                return false;
        }
        return true;
    }

    // ---- value -----------------------------------------------------------------

    bool value::is_integer() const noexcept
    {
        const double *d = std::get_if<double>(&m_data);
        return d != nullptr && *d == std::trunc(*d) && std::abs(*d) <= max_exact_integer;
    }

    std::optional<bool> value::as_bool() const noexcept
    {
        if (const bool *b = std::get_if<bool>(&m_data))
            return *b;
        return std::nullopt;
    }

    std::optional<double> value::as_number() const noexcept
    {
        if (const double *d = std::get_if<double>(&m_data))
            return *d;
        return std::nullopt;
    }

    std::optional<std::int64_t> value::as_integer() const noexcept
    {
        if (!is_integer())
            return std::nullopt;
        return static_cast<std::int64_t>(std::get<double>(m_data));
    }

    std::optional<std::string_view> value::as_string() const noexcept
    {
        if (const std::string *s = std::get_if<std::string>(&m_data))
            return std::string_view(*s);
        return std::nullopt;
    }

    const array *value::as_array() const noexcept
    {
        return std::get_if<array>(&m_data);
    }

    array *value::as_array() noexcept
    {
        return std::get_if<array>(&m_data);
    }

    const object *value::as_object() const noexcept
    {
        return std::get_if<object>(&m_data);
    }

    object *value::as_object() noexcept
    {
        return std::get_if<object>(&m_data);
    }

    bool value::bool_or(bool fallback) const noexcept
    {
        return as_bool().value_or(fallback);
    }

    double value::number_or(double fallback) const noexcept
    {
        return as_number().value_or(fallback);
    }

    std::int64_t value::integer_or(std::int64_t fallback) const noexcept
    {
        return as_integer().value_or(fallback);
    }

    std::string_view value::string_or(std::string_view fallback) const noexcept
    {
        return as_string().value_or(fallback);
    }

    std::size_t value::size() const noexcept
    {
        if (const array *a = as_array())
            return a->size();
        if (const object *o = as_object())
            return o->size();
        return 0;
    }

    const value *value::find(std::string_view key) const noexcept
    {
        const object *o = as_object();
        return o == nullptr ? nullptr : o->find(key);
    }

    value *value::find(std::string_view key) noexcept
    {
        object *o = as_object();
        return o == nullptr ? nullptr : o->find(key);
    }

    const value &value::at(std::string_view key) const noexcept
    {
        const value *found = find(key);
        return found == nullptr ? null_value() : *found;
    }

    const value &value::at(std::size_t index) const noexcept
    {
        const array *a = as_array();
        return a == nullptr || index >= a->size() ? null_value() : (*a)[index];
    }

    value &value::operator[](std::string_view key)
    {
        if (!is_object())
            m_data.emplace<object>();
        return std::get<object>(m_data)[key];
    }

    value &value::operator[](std::size_t index)
    {
        return std::get<array>(m_data)[index];
    }

    void value::push_back(value v)
    {
        if (!is_array())
            m_data.emplace<array>();
        std::get<array>(m_data).push_back(std::move(v));
    }

    bool value::operator==(const value &other) const
    {
        return m_data == other.m_data;
    }

    // ---- reading and writing -----------------------------------------------------

    std::string parse_error::message() const
    {
        return std::format("line {}, column {}: {}", line, column, reason);
    }

    std::expected<value, parse_error> parse(std::string_view text, const parse_options &options)
    {
        auto result = parser(text, options).run();
        if (result)
            return std::move(*result);

        // Line and column are worked out only on failure, so the parser's
        // hot loops do not keep count.
        parse_error error;
        error.offset = std::min(result.error().offset, text.size());
        error.reason = std::move(result.error().reason);
        error.line = 1;
        std::size_t line_start = 0;
        for (std::size_t i = 0; i < error.offset; ++i)
        {
            if (text[i] == '\n')
            {
                ++error.line;
                line_start = i + 1;
            }
        }
        error.column = 1 + width_of(text.substr(line_start, error.offset - line_start));
        if (error.line == 1 && text.starts_with("\xEF\xBB\xBF") && error.offset >= 3)
            --error.column; // the byte order mark is not a character anyone sees
        return std::unexpected(std::move(error));
    }

    std::string write(const value &v, const write_options &options)
    {
        return writer(options).run(v);
    }

    std::string write_compact(const value &v)
    {
        write_options options;
        options.pretty = false;
        return write(v, options);
    }

    // ---- files -------------------------------------------------------------------

    std::expected<std::string, std::string> read_text_file(const std::filesystem::path &path, std::uintmax_t limit)
    {
        const std::string shown = shown_name(path);
        std::error_code error;
        const std::uintmax_t size = std::filesystem::file_size(path, error);
        if (error)
            return std::unexpected(std::format("{}: cannot read the file ({})", shown, error.message()));
        if (size > limit)
            return std::unexpected(std::format("{}: the file is {} bytes, more than the {} allowed", shown, size, limit));
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return std::unexpected(std::format("{}: cannot open the file", shown));
        std::string bytes(static_cast<std::size_t>(size), '\0');
        file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (file.gcount() != static_cast<std::streamsize>(bytes.size()))
            return std::unexpected(std::format("{}: reading the file failed", shown));
        return bytes;
    }

    std::expected<void, std::string> write_text_file(const std::filesystem::path &path, std::string_view bytes)
    {
        const std::string shown = shown_name(path);
        // Next to the file, so the rename stays on one volume and is a
        // rename rather than a copy.
        std::filesystem::path temporary = path;
        temporary += ".partial";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (!file)
                return std::unexpected(std::format("{}: cannot create the file", shown));
            file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            file.flush();
            if (!file)
            {
                file.close();
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                return std::unexpected(std::format("{}: writing the file failed", shown));
            }
        }
        std::error_code error;
        std::filesystem::rename(temporary, path, error);
        if (error)
        {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return std::unexpected(std::format("{}: cannot replace the file ({})", shown, error.message()));
        }
        return {};
    }

    std::expected<value, std::string> read_file(const std::filesystem::path &path, const parse_options &options)
    {
        auto bytes = read_text_file(path);
        if (!bytes)
            return std::unexpected(std::move(bytes.error()));
        auto parsed = parse(*bytes, options);
        if (!parsed)
            return std::unexpected(shown_name(path) + ": " + parsed.error().message());
        return std::move(*parsed);
    }

    std::expected<void, std::string> write_file(const std::filesystem::path &path, const value &v,
                                                const write_options &options)
    {
        return write_text_file(path, write(v, options));
    }

} // namespace volt::json
