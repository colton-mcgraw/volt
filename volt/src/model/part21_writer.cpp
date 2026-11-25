#include <model/part21.hpp>

#include <charconv>
#include <cmath>
#include <format>

namespace volt::model::part21
{

    std::string format_real(double value)
    {
        // Part 21 has no infinity or NaN; the nearest it can say.
        if (std::isnan(value))
            value = 0.0;
        else if (std::isinf(value))
            value = value > 0.0 ? 1.0e308 : -1.0e308;

        // The shortest text that reads back to the same double.
        char buffer[64];
        const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), value);
        std::string_view text(buffer, error == std::errc{} ? static_cast<std::size_t>(end - buffer) : 0);

        std::string_view mantissa = text;
        std::string_view exponent;
        if (const std::size_t e = text.find_first_of("eE"); e != std::string_view::npos)
        {
            mantissa = text.substr(0, e);
            exponent = text.substr(e + 1);
        }

        std::string result(mantissa);
        if (result.find('.') == std::string::npos)
            result.push_back('.');
        if (!exponent.empty())
        {
            result.push_back('E');
            result.append(exponent);
        }
        return result;
    }

    namespace
    {
        // The next code point of a UTF-8 string. A byte that does not start a
        // valid sequence stands for itself, as Latin-1, so that any bytes at
        // all come out as something the standard allows.
        std::uint32_t next_code_point(std::string_view text, std::size_t &i)
        {
            const auto byte = [&](std::size_t at) { return static_cast<unsigned char>(text[at]); };
            const unsigned char first = byte(i);
            std::size_t length = 0;
            std::uint32_t code = 0;
            if (first < 0x80)
            {
                ++i;
                return first;
            }
            if ((first & 0xe0) == 0xc0)
            {
                length = 2;
                code = first & 0x1fu;
            }
            else if ((first & 0xf0) == 0xe0)
            {
                length = 3;
                code = first & 0x0fu;
            }
            else if ((first & 0xf8) == 0xf0)
            {
                length = 4;
                code = first & 0x07u;
            }
            if (length == 0 || i + length > text.size())
            {
                ++i;
                return first;
            }
            for (std::size_t k = 1; k < length; ++k)
            {
                if ((byte(i + k) & 0xc0) != 0x80)
                {
                    ++i;
                    return first;
                }
                code = (code << 6) | (byte(i + k) & 0x3fu);
            }
            i += length;
            return code;
        }

        constexpr char hex_digits[] = "0123456789ABCDEF";

        void append_hex(std::string &out, std::uint32_t value, int digits)
        {
            for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4)
                out.push_back(hex_digits[(value >> shift) & 0xf]);
        }
    } // namespace

    std::string format_string(std::string_view utf8)
    {
        std::string out;
        out.reserve(utf8.size() + 2);
        out.push_back('\'');

        std::size_t i = 0;
        while (i < utf8.size())
        {
            const std::uint32_t code = next_code_point(utf8, i);
            if (code == '\'')
                out.append("''");
            else if (code == '\\')
                out.append("\\\\");
            else if (code >= 0x20 && code <= 0x7e)
                out.push_back(static_cast<char>(code));
            else if (code < 0x20 || (code >= 0x7f && code <= 0xff && code < 0xa0))
            {
                // Control characters, one at a time.
                out.append("\\X\\");
                append_hex(out, code, 2);
            }
            else
            {
                // A run of characters beyond ASCII in one directive: \X2\ for
                // the Basic Multilingual Plane, \X4\ beyond it.
                const bool wide = code > 0xffff;
                out.append(wide ? "\\X4\\" : "\\X2\\");
                append_hex(out, code, wide ? 8 : 4);
                while (i < utf8.size())
                {
                    std::size_t next = i;
                    const std::uint32_t following = next_code_point(utf8, next);
                    if (following < 0xa0 || (following > 0xffff) != wide)
                        break;
                    append_hex(out, following, wide ? 8 : 4);
                    i = next;
                }
                out.append("\\X0\\");
            }
        }

        out.push_back('\'');
        return out;
    }

    namespace
    {
        void write_value(std::string &out, const parameter &value)
        {
            switch (value.type())
            {
            case kind::omitted:
                out.push_back('$');
                break;
            case kind::derived:
                out.push_back('*');
                break;
            case kind::integer:
            {
                char buffer[32];
                const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), value.integer());
                out.append(buffer, error == std::errc{} ? end : buffer);
                break;
            }
            case kind::real:
                out.append(format_real(value.number()));
                break;
            case kind::string:
                out.append(format_string(value.text()));
                break;
            case kind::enumeration:
                out.push_back('.');
                out.append(value.text());
                out.push_back('.');
                break;
            case kind::binary:
                out.push_back('"');
                out.append(value.text());
                out.push_back('"');
                break;
            case kind::reference:
                out.push_back('#');
                out.append(std::to_string(value.reference()));
                break;
            case kind::list:
            {
                out.push_back('(');
                const std::size_t count = value.size();
                for (std::size_t i = 0; i < count; ++i)
                {
                    if (i != 0)
                        out.push_back(',');
                    write_value(out, value[i]);
                }
                out.push_back(')');
                break;
            }
            case kind::typed:
            {
                out.append(value.type_name());
                out.push_back('(');
                write_value(out, value.inner());
                out.push_back(')');
                break;
            }
            }
        }

        void write_entity(std::string &out, const entity &entity)
        {
            const std::size_t parts = entity.part_count();
            if (entity.is_complex())
                out.push_back('(');
            for (std::size_t part = 0; part < parts; ++part)
            {
                out.append(entity.name(part));
                write_value(out, entity.parameters(part));
            }
            if (entity.is_complex())
                out.push_back(')');
            out.push_back(';');
        }
    } // namespace

    std::string write(const file &file, const write_options &options)
    {
        std::string out;
        out.reserve(file.size() * 64 + 1024);

        out.append("ISO-10303-21;");
        out.append(options.newline);
        out.append("HEADER;");
        out.append(options.newline);
        for (std::size_t i = 0; i < file.header_size(); ++i)
        {
            write_entity(out, file.header(i));
            out.append(options.newline);
        }
        out.append("ENDSEC;");
        out.append(options.newline);

        const std::size_t sections = file.section_count();
        for (std::size_t s = 0; s < sections; ++s)
        {
            const section section = file.get_section(s);
            out.append("DATA");
            if (section.parameters.is(kind::list))
                write_value(out, section.parameters);
            out.push_back(';');
            out.append(options.newline);
            for (std::size_t i = section.first; i < section.first + section.count; ++i)
            {
                const entity entity = file[i];
                out.push_back('#');
                out.append(std::to_string(entity.id()));
                out.push_back('=');
                write_entity(out, entity);
                out.append(options.newline);
            }
            out.append("ENDSEC;");
            out.append(options.newline);
        }
        if (sections == 0)
        {
            out.append("DATA;");
            out.append(options.newline);
            out.append("ENDSEC;");
            out.append(options.newline);
        }

        out.append("END-ISO-10303-21;");
        out.append(options.newline);
        return out;
    }

} // namespace volt::model::part21
