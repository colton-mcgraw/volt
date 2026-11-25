#include <model/part21.hpp>

#include <charconv>
#include <format>

// The physical file grammar of ISO 10303-21, read in one pass over the text
// with a hand-written scanner: no token objects, no copies except for the
// decoded text of strings. Lines are counted as the scanner goes, for errors.

namespace volt::model::part21
{

    namespace
    {
        // Lists and typed parameters nest this deep at most. Real files stay
        // below five (a B-spline surface's control points are a list of lists
        // of references); the limit only exists so that a hostile file cannot
        // exhaust the stack.
        constexpr std::size_t max_depth = 64;

        bool is_letter(char c) noexcept
        {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
        }

        bool is_digit(char c) noexcept
        {
            return c >= '0' && c <= '9';
        }

        int hex_digit(char c) noexcept
        {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            return -1;
        }

        void append_utf8(std::string &out, std::uint32_t code)
        {
            if (code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
                code = 0xfffd;
            if (code < 0x80)
                out.push_back(static_cast<char>(code));
            else if (code < 0x800)
            {
                out.push_back(static_cast<char>(0xc0 | (code >> 6)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
            }
            else if (code < 0x10000)
            {
                out.push_back(static_cast<char>(0xe0 | (code >> 12)));
                out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
            }
            else
            {
                out.push_back(static_cast<char>(0xf0 | (code >> 18)));
                out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
                out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
            }
        }

        class reader
        {
        public:
            explicit reader(std::string_view text) : m_p(text.data()), m_end(text.data() + text.size())
            {
                // A UTF-8 byte order mark, which some Windows tools write.
                if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF")
                    m_p += 3;
            }

            std::expected<file, std::string> run()
            {
                if (!read_file())
                    return std::unexpected(m_error);
                auto result = m_builder.finish();
                if (!result)
                    return std::unexpected(result.error());
                return result;
            }

        private:
            const char *m_p;
            const char *m_end;
            std::uint32_t m_line = 1;
            builder m_builder;
            std::string m_error;
            std::string m_keyword;
            std::string m_scratch;

            bool fail(std::string message)
            {
                if (m_error.empty())
                    m_error = std::format("line {}: {}", m_line, message);
                return false;
            }

            bool fail_at(std::uint32_t line, std::string message)
            {
                if (m_error.empty())
                    m_error = std::format("line {}: {}", line, message);
                return false;
            }

            bool at_end() const noexcept { return m_p >= m_end; }
            char peek() const noexcept { return m_p < m_end ? *m_p : '\0'; }

            std::string describe_here() const
            {
                if (at_end())
                    return "the end of the file";
                const char c = *m_p;
                if (c >= 0x21 && c <= 0x7e)
                    return std::format("'{}'", c);
                return std::format("byte 0x{:02X}", static_cast<unsigned char>(c));
            }

            // Whitespace and comments. False on an unterminated comment.
            bool skip()
            {
                while (m_p < m_end)
                {
                    const char c = *m_p;
                    if (c == '\n')
                    {
                        ++m_line;
                        ++m_p;
                    }
                    else if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v')
                        ++m_p;
                    else if (c == '/' && m_p + 1 < m_end && m_p[1] == '*')
                    {
                        const std::uint32_t start = m_line;
                        m_p += 2;
                        for (;;)
                        {
                            if (m_p + 1 >= m_end)
                                return fail_at(start, "a comment that never ends");
                            if (*m_p == '*' && m_p[1] == '/')
                            {
                                m_p += 2;
                                break;
                            }
                            if (*m_p == '\n')
                                ++m_line;
                            ++m_p;
                        }
                    }
                    else
                        break;
                }
                return true;
            }

            bool expect(char c, std::string_view what)
            {
                if (!skip())
                    return false;
                if (peek() != c)
                    return fail(std::format("expected '{}' {}, found {}", c, what, describe_here()));
                ++m_p;
                return true;
            }

            // A standard keyword (letters, digits, underscores) or a user
            // defined one (the same after '!'), upper-cased into m_keyword.
            bool read_keyword()
            {
                m_keyword.clear();
                if (peek() == '!')
                {
                    m_keyword.push_back('!');
                    ++m_p;
                }
                if (!is_letter(peek()))
                    return fail(std::format("expected a keyword, found {}", describe_here()));
                while (m_p < m_end && (is_letter(*m_p) || is_digit(*m_p)))
                {
                    char c = *m_p++;
                    if (c >= 'a' && c <= 'z')
                        c = static_cast<char>(c - 'a' + 'A');
                    m_keyword.push_back(c);
                }
                return true;
            }

            // The section keywords, which may contain '-' (END-ISO-10303-21).
            std::string_view read_section_keyword()
            {
                const char *start = m_p;
                while (m_p < m_end && (is_letter(*m_p) || is_digit(*m_p) || *m_p == '-'))
                    ++m_p;
                return std::string_view(start, static_cast<std::size_t>(m_p - start));
            }

            bool read_file()
            {
                if (!skip())
                    return false;
                if (read_section_keyword() != "ISO-10303-21")
                    return fail("not a STEP file: it does not start with ISO-10303-21;");
                if (!expect(';', "after ISO-10303-21"))
                    return false;

                if (!skip())
                    return false;
                if (read_section_keyword() != "HEADER")
                    return fail("expected HEADER;");
                if (!expect(';', "after HEADER"))
                    return false;
                if (!read_header())
                    return false;

                for (;;)
                {
                    if (!skip())
                        return false;
                    if (at_end())
                        return fail("the file ends without END-ISO-10303-21;");
                    const std::uint32_t line = m_line;
                    const std::string_view keyword = read_section_keyword();
                    if (keyword == "DATA")
                    {
                        if (!read_data())
                            return false;
                    }
                    else if (keyword == "END-ISO-10303-21")
                    {
                        // Whatever follows the end (a signature, padding) is
                        // not part of the exchange structure.
                        return expect(';', "after END-ISO-10303-21");
                    }
                    else if (keyword == "ANCHOR" || keyword == "REFERENCE" || keyword == "SIGNATURE")
                    {
                        // Edition 3 sections for external references. Nothing
                        // here resolves those; read past them.
                        if (!skip_section())
                            return false;
                    }
                    else if (keyword.empty())
                        return fail(std::format("expected a section, found {}", describe_here()));
                    else
                        return fail_at(line, std::format("unknown section {}", keyword));
                }
            }

            bool read_header()
            {
                for (;;)
                {
                    if (!skip())
                        return false;
                    if (at_end())
                        return fail("the header never ends (no ENDSEC;)");
                    if (m_end - m_p >= 6 && std::string_view(m_p, 6) == "ENDSEC")
                    {
                        m_p += 6;
                        return expect(';', "after ENDSEC");
                    }
                    const std::uint32_t line = m_line;
                    if (!read_keyword())
                        return false;
                    m_builder.begin_header(m_keyword);
                    if (!expect('(', "after a header entity's name") || !read_parameters(0))
                        return false;
                    m_builder.end_entity();
                    if (!expect(';', std::format("after the header entity at line {}", line)))
                        return false;
                }
            }

            bool read_data()
            {
                m_builder.begin_section();
                if (!skip())
                    return false;
                if (peek() == '(')
                {
                    ++m_p;
                    m_builder.begin_section_parameters();
                    if (!read_parameters(0))
                        return false;
                    m_builder.end_section_parameters();
                }
                if (!expect(';', "after DATA"))
                    return false;

                for (;;)
                {
                    if (!skip())
                        return false;
                    if (at_end())
                        return fail("the DATA section never ends (no ENDSEC;)");
                    if (*m_p == '#')
                    {
                        if (!read_instance())
                            return false;
                        continue;
                    }
                    if (m_end - m_p >= 6 && std::string_view(m_p, 6) == "ENDSEC")
                    {
                        m_p += 6;
                        return expect(';', "after ENDSEC");
                    }
                    return fail(std::format("expected an instance (#...) or ENDSEC, found {}", describe_here()));
                }
            }

            bool skip_section()
            {
                // Strings may contain anything, ENDSEC included.
                while (m_p < m_end)
                {
                    const char c = *m_p;
                    if (c == '\'')
                    {
                        ++m_p;
                        while (m_p < m_end && *m_p != '\'')
                        {
                            if (*m_p == '\n')
                                ++m_line;
                            ++m_p;
                        }
                        if (m_p < m_end)
                            ++m_p;
                        continue;
                    }
                    if (c == '\n')
                        ++m_line;
                    if (c == 'E' && m_end - m_p >= 6 && std::string_view(m_p, 6) == "ENDSEC")
                    {
                        m_p += 6;
                        return expect(';', "after ENDSEC");
                    }
                    ++m_p;
                }
                return fail("a section never ends (no ENDSEC;)");
            }

            bool read_id(std::uint64_t &id)
            {
                // At '#'.
                ++m_p;
                if (!is_digit(peek()))
                    return fail(std::format("expected an instance number after '#', found {}", describe_here()));
                const char *start = m_p;
                while (m_p < m_end && is_digit(*m_p))
                    ++m_p;
                const auto [end, error] = std::from_chars(start, m_p, id);
                if (error != std::errc{} || end != m_p || id == 0)
                    return fail(std::format("invalid instance number #{}", std::string_view(start, m_p)));
                return true;
            }

            bool read_instance()
            {
                const std::uint32_t line = m_line;
                std::uint64_t id = 0;
                if (!read_id(id))
                    return false;
                if (!expect('=', std::format("after #{}", id)) || !skip())
                    return false;

                if (peek() == '(')
                {
                    // A complex instance: its types in a row.
                    ++m_p;
                    m_builder.begin_instance(id, line, true);
                    std::size_t parts = 0;
                    for (;;)
                    {
                        if (!skip())
                            return false;
                        if (peek() == ')')
                        {
                            ++m_p;
                            break;
                        }
                        if (!read_keyword())
                            return false;
                        m_builder.begin_part(m_keyword);
                        if (!expect('(', std::format("after {} in #{}", m_keyword, id)) || !read_parameters(0))
                            return false;
                        m_builder.end_part();
                        ++parts;
                    }
                    if (parts == 0)
                        return fail_at(line, std::format("#{} is a complex instance without types", id));
                    m_builder.end_entity();
                }
                else
                {
                    if (!read_keyword())
                        return false;
                    m_builder.begin_instance(id, line, false);
                    m_builder.begin_part(m_keyword);
                    if (!expect('(', std::format("after {} in #{}", m_keyword, id)) || !read_parameters(0))
                        return false;
                    m_builder.end_part();
                    m_builder.end_entity();
                }
                return expect(';', std::format("after #{}", id));
            }

            // After the opening parenthesis: parameters up to and including
            // the closing one.
            bool read_parameters(std::size_t depth)
            {
                if (!skip())
                    return false;
                if (peek() == ')')
                {
                    ++m_p;
                    return true;
                }
                for (;;)
                {
                    if (!read_value(depth))
                        return false;
                    if (!skip())
                        return false;
                    const char c = peek();
                    ++m_p;
                    if (c == ',')
                        continue;
                    if (c == ')')
                        return true;
                    --m_p;
                    return fail(std::format("expected ',' or ')' between parameters, found {}", describe_here()));
                }
            }

            bool read_value(std::size_t depth)
            {
                if (!skip())
                    return false;
                if (at_end())
                    return fail("the file ends inside an instance");

                const char c = *m_p;
                switch (c)
                {
                case '$':
                    ++m_p;
                    m_builder.omitted();
                    return true;
                case '*':
                    ++m_p;
                    m_builder.derived();
                    return true;
                case '#':
                {
                    std::uint64_t id = 0;
                    if (!read_id(id))
                        return false;
                    m_builder.reference(id);
                    return true;
                }
                case '\'':
                    return read_string();
                case '"':
                    return read_binary();
                case '(':
                    if (depth + 1 > max_depth)
                        return fail("lists nest too deeply");
                    ++m_p;
                    m_builder.begin_list();
                    if (!read_parameters(depth + 1))
                        return false;
                    m_builder.end_list();
                    return true;
                case '.':
                    if (m_p + 1 < m_end && is_digit(m_p[1]))
                        return read_number();
                    return read_enumeration();
                default:
                    break;
                }

                if (is_digit(c) || c == '+' || c == '-')
                    return read_number();

                if (is_letter(c) || c == '!')
                {
                    // A typed parameter: TYPE(value).
                    if (depth + 1 > max_depth)
                        return fail("typed parameters nest too deeply");
                    if (!read_keyword())
                        return false;
                    m_builder.begin_typed(m_keyword);
                    if (!expect('(', "after a typed parameter's type"))
                        return false;
                    if (!read_value(depth + 1) || !expect(')', "after a typed parameter's value"))
                        return false;
                    m_builder.end_typed();
                    return true;
                }

                return fail(std::format("expected a parameter, found {}", describe_here()));
            }

            bool read_number()
            {
                const char *start = m_p;
                if (*m_p == '+' || *m_p == '-')
                    ++m_p;
                bool real = false;
                std::size_t digits = 0;
                while (m_p < m_end && is_digit(*m_p))
                {
                    ++m_p;
                    ++digits;
                }
                if (m_p < m_end && *m_p == '.')
                {
                    real = true;
                    ++m_p;
                    while (m_p < m_end && is_digit(*m_p))
                    {
                        ++m_p;
                        ++digits;
                    }
                }
                if (digits == 0)
                    return fail("a sign or a point without digits where a number was expected");
                if (m_p < m_end && (*m_p == 'E' || *m_p == 'e'))
                {
                    real = true;
                    ++m_p;
                    if (m_p < m_end && (*m_p == '+' || *m_p == '-'))
                        ++m_p;
                    if (!is_digit(peek()))
                        return fail("an exponent without digits");
                    while (m_p < m_end && is_digit(*m_p))
                        ++m_p;
                }

                // from_chars takes no leading '+'.
                const char *first = *start == '+' ? start + 1 : start;
                if (!real)
                {
                    std::int64_t value = 0;
                    const auto [end, error] = std::from_chars(first, m_p, value);
                    if (error == std::errc{} && end == m_p)
                    {
                        m_builder.integer(value);
                        return true;
                    }
                    // Too large for 64 bits: keep it as a real rather than fail.
                }

                // A leading point (".5", "-.5") is not standard but is written
                // by some exporters, and from_chars takes it as it is.
                double value = 0.0;
                const auto [end, error] = std::from_chars(first, m_p, value);
                if (end != m_p || (error != std::errc{} && error != std::errc::result_out_of_range))
                    return fail(std::format("invalid number {}", std::string_view(start, m_p)));
                m_builder.real(value);
                return true;
            }

            bool read_enumeration()
            {
                ++m_p; // '.'
                const char *start = m_p;
                while (m_p < m_end && (is_letter(*m_p) || is_digit(*m_p)))
                    ++m_p;
                if (m_p == start || peek() != '.')
                    return fail("an enumeration value must be written .NAME.");
                m_scratch.assign(start, m_p);
                for (char &c : m_scratch)
                {
                    if (c >= 'a' && c <= 'z')
                        c = static_cast<char>(c - 'a' + 'A');
                }
                ++m_p;
                m_builder.enumeration(m_scratch);
                return true;
            }

            bool read_binary()
            {
                const std::uint32_t line = m_line;
                ++m_p; // '"'
                const char *start = m_p;
                while (m_p < m_end && *m_p != '"')
                {
                    if (hex_digit(*m_p) < 0)
                        return fail("a binary value may only hold hexadecimal digits");
                    ++m_p;
                }
                if (at_end())
                    return fail_at(line, "a binary value that never ends");
                if (m_p == start || *start < '0' || *start > '3')
                    return fail("a binary value must start with the count of unused bits, 0 to 3");
                const std::string_view hex(start, static_cast<std::size_t>(m_p - start));
                ++m_p;
                m_builder.binary(hex);
                return true;
            }

            // Reads `count` hex digits as one number.
            bool read_hex(std::size_t count, std::uint32_t &value)
            {
                if (static_cast<std::size_t>(m_end - m_p) < count)
                    return false;
                value = 0;
                for (std::size_t i = 0; i < count; ++i)
                {
                    const int digit = hex_digit(m_p[i]);
                    if (digit < 0)
                        return false;
                    value = value * 16 + static_cast<std::uint32_t>(digit);
                }
                m_p += count;
                return true;
            }

            bool starts_with(std::string_view text) const noexcept
            {
                return static_cast<std::size_t>(m_end - m_p) >= text.size() && std::string_view(m_p, text.size()) == text;
            }

            bool read_string()
            {
                const std::uint32_t line = m_line;
                ++m_p; // '\''
                m_scratch.clear();

                for (;;)
                {
                    if (m_p >= m_end)
                        return fail_at(line, "a string that never ends");
                    const char c = *m_p;

                    if (c == '\'')
                    {
                        if (m_p + 1 < m_end && m_p[1] == '\'')
                        {
                            m_scratch.push_back('\'');
                            m_p += 2;
                            continue;
                        }
                        ++m_p;
                        break;
                    }

                    // Line breaks inside a string are where a writer wrapped
                    // a long line; they are not part of the value.
                    if (c == '\n')
                    {
                        ++m_line;
                        ++m_p;
                        continue;
                    }
                    if (c == '\r')
                    {
                        ++m_p;
                        continue;
                    }

                    if (c != '\\')
                    {
                        m_scratch.push_back(c);
                        ++m_p;
                        continue;
                    }

                    if (!read_escape())
                        return false;
                }

                m_builder.string(m_scratch);
                return true;
            }

            // At a backslash in a string.
            bool read_escape()
            {
                if (starts_with("\\\\"))
                {
                    m_scratch.push_back('\\');
                    m_p += 2;
                    return true;
                }
                if (starts_with("\\S\\") && m_end - m_p >= 4)
                {
                    // The character with the high bit set, in the current code
                    // page; ISO 8859-1 (page A) unless \P?\ changed it, which is
                    // taken to be Latin-1 as well.
                    const auto code = static_cast<std::uint32_t>(static_cast<unsigned char>(m_p[3]) & 0x7f) + 0x80;
                    append_utf8(m_scratch, code);
                    m_p += 4;
                    return true;
                }
                if (m_end - m_p >= 4 && m_p[1] == 'P' && m_p[3] == '\\' && m_p[2] >= 'A' && m_p[2] <= 'I')
                {
                    m_p += 4;
                    return true;
                }
                if (starts_with("\\X\\"))
                {
                    m_p += 3;
                    std::uint32_t code = 0;
                    if (!read_hex(2, code))
                        return fail("\\X\\ must be followed by two hexadecimal digits");
                    append_utf8(m_scratch, code);
                    return true;
                }
                if (starts_with("\\X2\\") || starts_with("\\X4\\"))
                {
                    const std::size_t width = m_p[2] == '2' ? 4 : 8;
                    m_p += 4;
                    std::uint32_t high_surrogate = 0;
                    for (;;)
                    {
                        if (starts_with("\\X0\\"))
                        {
                            m_p += 4;
                            break;
                        }
                        std::uint32_t code = 0;
                        if (!read_hex(width, code))
                            return fail("\\X2\\ or \\X4\\ must hold groups of hexadecimal digits and end with \\X0\\");
                        if (width == 4 && code >= 0xd800 && code <= 0xdbff)
                        {
                            high_surrogate = code;
                            continue;
                        }
                        if (width == 4 && code >= 0xdc00 && code <= 0xdfff && high_surrogate != 0)
                            code = 0x10000 + ((high_surrogate - 0xd800) << 10) + (code - 0xdc00);
                        high_surrogate = 0;
                        append_utf8(m_scratch, code);
                    }
                    return true;
                }
                if (starts_with("\\N\\"))
                {
                    m_scratch.push_back('\n');
                    m_p += 3;
                    return true;
                }
                if (starts_with("\\F\\"))
                {
                    m_p += 3;
                    return true;
                }

                // A lone backslash: not allowed by the standard, but meant
                // literally by whoever wrote it.
                m_scratch.push_back('\\');
                ++m_p;
                return true;
            }
        };
    } // namespace

    std::expected<file, std::string> parse(std::string_view text)
    {
        reader reader(text);
        return reader.run();
    }

} // namespace volt::model::part21
