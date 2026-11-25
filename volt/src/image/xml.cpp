#include <image/xml.hpp>

#include <algorithm>
#include <charconv>
#include <format>
#include <map>

namespace volt::xml
{

    namespace
    {
        bool is_space(char c)
        {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r';
        }

        bool is_name_start(char c)
        {
            const auto u = static_cast<unsigned char>(c);
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':' || u >= 0x80;
        }

        bool is_name_char(char c)
        {
            return is_name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
        }

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

        using failure = std::unexpected<std::string>;

        class parser
        {
        public:
            parser(std::string_view text, const parse_options &options) : m_text(text), m_options(options) {}

            std::expected<element, std::string> run()
            {
                if (m_text.starts_with("\xEF\xBB\xBF"))
                    m_position = 3;

                std::vector<element> open;
                element root;
                bool have_root = false;

                while (m_position < m_text.size())
                {
                    if (m_text[m_position] != '<')
                    {
                        const std::size_t end = std::min(m_text.find('<', m_position), m_text.size());
                        const std::string_view run = m_text.substr(m_position, end - m_position);
                        if (!open.empty())
                        {
                            if (auto decoded = decode(run, open.back().text, false); !decoded)
                                return failure(std::move(decoded.error()));
                        }
                        else if (!std::all_of(run.begin(), run.end(), is_space))
                            return error(have_root ? "text after the root element" : "text before the root element");
                        m_position = end;
                        continue;
                    }

                    const std::string_view rest = m_text.substr(m_position);
                    if (rest.starts_with("<?"))
                    {
                        if (!skip_past("?>"))
                            return error("an unterminated processing instruction");
                    }
                    else if (rest.starts_with("<!--"))
                    {
                        if (!skip_past("-->"))
                            return error("an unterminated comment");
                    }
                    else if (rest.starts_with("<![CDATA["))
                    {
                        const std::size_t start = m_position + 9;
                        const std::size_t end = m_text.find("]]>", start);
                        if (end == std::string_view::npos)
                            return error("an unterminated CDATA section");
                        if (open.empty())
                            return error("a CDATA section outside the root element");
                        open.back().text.append(m_text.substr(start, end - start));
                        m_position = end + 3;
                    }
                    else if (rest.starts_with("<!DOCTYPE"))
                    {
                        if (auto doctype = read_doctype(); !doctype)
                            return failure(std::move(doctype.error()));
                    }
                    else if (rest.starts_with("<!"))
                    {
                        if (!skip_past(">"))
                            return error("an unterminated declaration");
                    }
                    else if (rest.starts_with("</"))
                    {
                        m_position += 2;
                        const std::string_view name = read_name();
                        skip_spaces();
                        if (m_position >= m_text.size() || m_text[m_position] != '>')
                            return error("a malformed end tag");
                        ++m_position;
                        if (open.empty())
                            return error(std::format("the end tag </{}> without a start tag", name));
                        if (open.back().name != name)
                            return error(std::format("the end tag </{}> where </{}> (from line {}) belongs", name,
                                                     open.back().name, open.back().line));
                        element done = std::move(open.back());
                        open.pop_back();
                        if (open.empty())
                        {
                            root = std::move(done);
                            have_root = true;
                        }
                        else
                            open.back().children.push_back(std::move(done));
                    }
                    else
                    {
                        if (have_root && open.empty())
                            return error("a second root element");
                        element started;
                        started.line = line_at(m_position);
                        ++m_position;
                        started.name = std::string(read_name());
                        if (started.name.empty())
                            return error("a '<' that starts no tag");

                        bool closed = false;
                        for (;;)
                        {
                            skip_spaces();
                            if (m_position >= m_text.size())
                                return error(std::format("the start tag <{}> is not finished", started.name));
                            if (m_text.substr(m_position).starts_with("/>"))
                            {
                                m_position += 2;
                                closed = true;
                                break;
                            }
                            if (m_text[m_position] == '>')
                            {
                                ++m_position;
                                break;
                            }
                            attribute a;
                            a.name = std::string(read_name());
                            if (a.name.empty())
                                return error(std::format("a malformed attribute in <{}>", started.name));
                            skip_spaces();
                            if (m_position >= m_text.size() || m_text[m_position] != '=')
                                return error(std::format("the attribute {} in <{}> has no value", a.name, started.name));
                            ++m_position;
                            skip_spaces();
                            if (m_position >= m_text.size() || (m_text[m_position] != '"' && m_text[m_position] != '\''))
                                return error(std::format("the value of {} in <{}> is not quoted", a.name, started.name));
                            const char quote = m_text[m_position++];
                            const std::size_t end = m_text.find(quote, m_position);
                            if (end == std::string_view::npos)
                                return error(std::format("the value of {} in <{}> is not closed", a.name, started.name));
                            if (auto decoded = decode(m_text.substr(m_position, end - m_position), a.value, true);
                                !decoded)
                                return failure(std::move(decoded.error()));
                            m_position = end + 1;
                            started.attributes.push_back(std::move(a));
                        }

                        if (closed)
                        {
                            if (open.empty())
                            {
                                root = std::move(started);
                                have_root = true;
                            }
                            else
                                open.back().children.push_back(std::move(started));
                        }
                        else
                        {
                            if (open.size() >= m_options.max_depth)
                                return error(std::format("elements nested more than {} deep", m_options.max_depth));
                            open.push_back(std::move(started));
                        }
                    }
                }

                if (!open.empty())
                    return failure(std::format("the XML ends inside <{}> (opened on line {})", open.back().name,
                                               open.back().line));
                if (!have_root)
                    return failure(std::string("the XML has no root element"));
                return root;
            }

        private:
            std::unexpected<std::string> error(std::string_view what)
            {
                return failure(std::format("XML error on line {}: {}", line_at(m_position), what));
            }

            std::size_t line_at(std::size_t position)
            {
                position = std::min(position, m_text.size());
                if (position < m_line_position)
                {
                    m_line_position = 0;
                    m_line = 1;
                }
                m_line += static_cast<std::size_t>(
                    std::count(m_text.begin() + static_cast<std::ptrdiff_t>(m_line_position),
                               m_text.begin() + static_cast<std::ptrdiff_t>(position), '\n'));
                m_line_position = position;
                return m_line;
            }

            void skip_spaces()
            {
                while (m_position < m_text.size() && is_space(m_text[m_position]))
                    ++m_position;
            }

            bool skip_past(std::string_view terminator)
            {
                const std::size_t end = m_text.find(terminator, m_position);
                if (end == std::string_view::npos)
                {
                    m_position = m_text.size();
                    return false;
                }
                m_position = end + terminator.size();
                return true;
            }

            std::string_view read_name()
            {
                const std::size_t start = m_position;
                if (m_position < m_text.size() && is_name_start(m_text[m_position]))
                    while (m_position < m_text.size() && is_name_char(m_text[m_position]))
                        ++m_position;
                return m_text.substr(start, m_position - start);
            }

            // Appends `raw` with references replaced. Attribute values also
            // get their whitespace normalized, as XML says.
            std::expected<void, std::string> decode(std::string_view raw, std::string &out, bool attribute_value)
            {
                for (std::size_t i = 0; i < raw.size(); ++i)
                {
                    const char c = raw[i];
                    if (c != '&')
                    {
                        out += attribute_value && is_space(c) ? ' ' : c;
                        continue;
                    }
                    const std::size_t end = raw.find(';', i);
                    if (end == std::string_view::npos || end - i > 64)
                    {
                        out += '&'; // a stray ampersand: kept, leniently
                        continue;
                    }
                    const std::string_view reference = raw.substr(i + 1, end - i - 1);
                    if (!expand(reference, out))
                    {
                        out.append(raw.substr(i, end - i + 1)); // unknown: kept as written
                    }
                    i = end;
                    if (m_expanded > m_options.max_entity_expansion)
                        return error("entity references expand to too much text");
                }
                return {};
            }

            bool expand(std::string_view reference, std::string &out)
            {
                if (reference.starts_with('#'))
                {
                    const bool hex = reference.size() > 1 && (reference[1] == 'x' || reference[1] == 'X');
                    const std::string_view digits = reference.substr(hex ? 2 : 1);
                    std::uint32_t code = 0;
                    const auto [end, problem] =
                        std::from_chars(digits.data(), digits.data() + digits.size(), code, hex ? 16 : 10);
                    if (problem != std::errc{} || end != digits.data() + digits.size() || digits.empty() ||
                        code > 0x10FFFF || code == 0)
                        return false;
                    append_utf8(out, code);
                    return true;
                }
                if (reference == "lt")
                    out += '<';
                else if (reference == "gt")
                    out += '>';
                else if (reference == "amp")
                    out += '&';
                else if (reference == "quot")
                    out += '"';
                else if (reference == "apos")
                    out += '\'';
                else
                {
                    const auto found = m_entities.find(std::string(reference));
                    if (found == m_entities.end())
                        return false;
                    out += found->second;
                    m_expanded += found->second.size();
                }
                return true;
            }

            // <!DOCTYPE name ... [ internal subset ]>: entity declarations
            // are kept, everything else skipped.
            std::expected<void, std::string> read_doctype()
            {
                m_position += 9;
                while (m_position < m_text.size() && m_text[m_position] != '[' && m_text[m_position] != '>')
                {
                    const char c = m_text[m_position++];
                    if (c == '"' || c == '\'')
                    {
                        const std::size_t end = m_text.find(c, m_position);
                        if (end == std::string_view::npos)
                            return error("an unterminated DOCTYPE");
                        m_position = end + 1;
                    }
                }
                if (m_position >= m_text.size())
                    return error("an unterminated DOCTYPE");
                if (m_text[m_position] == '>')
                {
                    ++m_position;
                    return {};
                }

                ++m_position; // '['
                for (;;)
                {
                    skip_spaces();
                    if (m_position >= m_text.size())
                        return error("an unterminated DOCTYPE");
                    const std::string_view rest = m_text.substr(m_position);
                    if (rest.starts_with(']'))
                    {
                        ++m_position;
                        skip_spaces();
                        if (m_position >= m_text.size() || m_text[m_position] != '>')
                            return error("a malformed DOCTYPE");
                        ++m_position;
                        return {};
                    }
                    if (rest.starts_with("<!--"))
                    {
                        if (!skip_past("-->"))
                            return error("an unterminated comment");
                    }
                    else if (rest.starts_with("<?"))
                    {
                        if (!skip_past("?>"))
                            return error("an unterminated processing instruction");
                    }
                    else if (rest.starts_with("<!ENTITY"))
                    {
                        m_position += 8;
                        skip_spaces();
                        const bool parameter = m_position < m_text.size() && m_text[m_position] == '%';
                        if (parameter)
                        {
                            ++m_position;
                            skip_spaces();
                        }
                        const std::string name(read_name());
                        skip_spaces();
                        if (m_position < m_text.size() && (m_text[m_position] == '"' || m_text[m_position] == '\''))
                        {
                            const char quote = m_text[m_position++];
                            const std::size_t end = m_text.find(quote, m_position);
                            if (end == std::string_view::npos)
                                return error("an unterminated entity declaration");
                            std::string value;
                            // Character references only: one level, no
                            // recursion into other entities.
                            const std::string_view raw = m_text.substr(m_position, end - m_position);
                            for (std::size_t i = 0; i < raw.size(); ++i)
                            {
                                const std::size_t semicolon = raw.find(';', i);
                                if (raw[i] == '&' && i + 1 < raw.size() && raw[i + 1] == '#' &&
                                    semicolon != std::string_view::npos &&
                                    expand(raw.substr(i + 1, semicolon - i - 1), value))
                                    i = semicolon;
                                else
                                    value += raw[i];
                            }
                            if (!parameter && !name.empty() && !m_entities.contains(name))
                                m_entities.emplace(name, std::move(value));
                            m_position = end + 1;
                        }
                        if (!skip_declaration())
                            return error("an unterminated entity declaration");
                    }
                    else if (rest.starts_with('%'))
                    {
                        if (!skip_past(";"))
                            return error("a malformed DOCTYPE");
                    }
                    else if (rest.starts_with("<!"))
                    {
                        if (!skip_declaration())
                            return error("an unterminated declaration in the DOCTYPE");
                    }
                    else
                        return error("a malformed DOCTYPE");
                }
            }

            // To just past the '>' that ends a declaration, minding quotes.
            bool skip_declaration()
            {
                while (m_position < m_text.size())
                {
                    const char c = m_text[m_position++];
                    if (c == '>')
                        return true;
                    if (c == '"' || c == '\'')
                    {
                        const std::size_t end = m_text.find(c, m_position);
                        if (end == std::string_view::npos)
                            return false;
                        m_position = end + 1;
                    }
                }
                return false;
            }

            std::string_view m_text;
            parse_options m_options;
            std::size_t m_position = 0;
            std::size_t m_line = 1;
            std::size_t m_line_position = 0;
            std::map<std::string, std::string> m_entities;
            std::size_t m_expanded = 0;
        };
    } // namespace

    std::expected<element, std::string> parse(std::string_view text, const parse_options &options)
    {
        return parser(text, options).run();
    }

} // namespace volt::xml
