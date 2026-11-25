#include <library/library.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <unordered_map>

namespace volt::library
{

    namespace
    {
        std::string utf8_name(const std::filesystem::path &path)
        {
            const std::u8string name = path.filename().u8string();
            return std::string(name.begin(), name.end());
        }

        void put(json::object &o, std::string_view key, const std::string &text)
        {
            if (!text.empty())
                o.set(key, json::value(text));
        }

        constexpr std::string_view known_keys[] = {
            "format", "format_version", "name", "title", "description", "version", "author", "license", "url", "parts", "files",
        };

        bool is_known(std::string_view key) noexcept
        {
            for (const std::string_view k : known_keys)
                if (k == key)
                    return true;
            return false;
        }

        struct string_hash
        {
            using is_transparent = void;
            std::size_t operator()(std::string_view text) const noexcept { return std::hash<std::string_view>{}(text); }
        };

        constexpr std::string_view base64_digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        std::string base64_encode(std::string_view bytes)
        {
            std::string text;
            text.reserve((bytes.size() + 2) / 3 * 4);
            for (std::size_t i = 0; i < bytes.size(); i += 3)
            {
                const std::size_t left = bytes.size() - i;
                const std::uint32_t a = static_cast<unsigned char>(bytes[i]);
                const std::uint32_t b = left > 1 ? static_cast<unsigned char>(bytes[i + 1]) : 0u;
                const std::uint32_t c = left > 2 ? static_cast<unsigned char>(bytes[i + 2]) : 0u;
                const std::uint32_t group = a << 16 | b << 8 | c;
                text += base64_digits[group >> 18 & 63];
                text += base64_digits[group >> 12 & 63];
                text += left > 1 ? base64_digits[group >> 6 & 63] : '=';
                text += left > 2 ? base64_digits[group & 63] : '=';
            }
            return text;
        }

        // Standard base64, padding optional, whitespace skipped.
        std::optional<std::string> base64_decode(std::string_view text)
        {
            std::string bytes;
            bytes.reserve(text.size() / 4 * 3);
            std::uint32_t group = 0;
            int bits = 0;
            bool padded = false;
            for (const char ch : text)
            {
                if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t')
                    continue;
                if (ch == '=')
                {
                    padded = true;
                    continue;
                }
                const std::size_t digit = base64_digits.find(ch);
                if (digit == std::string_view::npos || padded)
                    return std::nullopt;
                group = (group << 6 | static_cast<std::uint32_t>(digit)) & 0xFFFFFFu;
                bits += 6;
                if (bits >= 8)
                {
                    bits -= 8;
                    bytes += static_cast<char>(group >> bits & 0xFF);
                }
            }
            // What is left over is padding bits, which must be zero.
            if (bits >= 6 || (group & ((1u << bits) - 1u)) != 0)
                return std::nullopt;
            return bytes;
        }

        // Whether the bytes can be a JSON string as they are and still read
        // well in a diff: valid UTF-8 with no control characters but line
        // ends and tabs.
        bool is_text(std::string_view bytes) noexcept
        {
            std::size_t i = 0;
            while (i < bytes.size())
            {
                const unsigned char c = static_cast<unsigned char>(bytes[i]);
                if (c < 0x80)
                {
                    if ((c < 0x20 && c != '\n' && c != '\r' && c != '\t') || c == 0x7F)
                        return false;
                    ++i;
                    continue;
                }
                const std::size_t length = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 ? 2 : 0;
                if (length == 0 || c > 0xF4 || i + length > bytes.size())
                    return false;
                std::uint32_t code = c & (0xFFu >> (length + 1));
                for (std::size_t k = 1; k < length; ++k)
                {
                    const unsigned char next = static_cast<unsigned char>(bytes[i + k]);
                    if ((next & 0xC0) != 0x80)
                        return false;
                    code = code << 6 | (next & 0x3Fu);
                }
                const std::uint32_t least = length == 4 ? 0x10000u : length == 3 ? 0x800u : 0x80u;
                if (code < least || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
                    return false;
                i += length;
            }
            return true;
        }
    } // namespace

    // ---- embedded files ----------------------------------------------------------------

    bool valid_file_name(std::string_view name) noexcept
    {
        if (name.empty())
            return false;
        for (const char c : name)
            if (c == ':' || c == '\\' || static_cast<unsigned char>(c) < 0x20 || c == 0x7F)
                return false;
        while (true)
        {
            const std::size_t slash = name.find('/');
            const std::string_view segment = name.substr(0, slash);
            if (segment.empty() || segment == "." || segment == ".." || segment.front() == ' ' || segment.back() == ' ')
                return false;
            if (slash == std::string_view::npos)
                return true;
            name.remove_prefix(slash + 1);
        }
    }

    std::string content_hash(std::string_view bytes)
    {
        std::uint64_t hash = 14695981039346656037ull;
        for (const char c : bytes)
        {
            hash ^= static_cast<unsigned char>(c);
            hash *= 1099511628211ull;
        }
        return std::format("{:016x}", hash);
    }

    json::value file_to_json(const embedded_file &file)
    {
        json::object o;
        o.set("name", json::value(file.name));
        const bool text = is_text(file.data);
        o.set("encoding", json::value(text ? "text" : "base64"));
        o.set("data", json::value(text ? file.data : base64_encode(file.data)));
        for (const json::member &m : file.extra)
            if (!o.contains(m.key))
                o.set(m.key, m.value);
        return json::value(std::move(o));
    }

    std::optional<embedded_file> file_from_json(const json::value &v, std::string &problem)
    {
        const json::object *o = v.as_object();
        if (o == nullptr)
        {
            problem = std::format("a file should be an object, not {}", json::describe(v.type()));
            return std::nullopt;
        }
        embedded_file file;
        file.name = std::string(v.at("name").string_or());
        if (!valid_file_name(file.name))
        {
            problem = file.name.empty() ? std::string("a file has no \"name\"")
                                        : std::format("\"{}\" cannot be a file's name: it must be a relative path "
                                                      "with forward slashes and no \"..\"",
                                                      file.name);
            return std::nullopt;
        }
        const auto data = v.at("data").as_string();
        if (!data)
        {
            problem = std::format("the file \"{}\" has no \"data\"", file.name);
            return std::nullopt;
        }
        const std::string_view encoding = v.at("encoding").string_or("text");
        if (encoding == "text")
            file.data = std::string(*data);
        else if (encoding == "base64")
        {
            auto bytes = base64_decode(*data);
            if (!bytes)
            {
                problem = std::format("the data of the file \"{}\" is not base64", file.name);
                return std::nullopt;
            }
            file.data = std::move(*bytes);
        }
        else
        {
            problem = std::format("the file \"{}\" has the encoding \"{}\"; \"text\" and \"base64\" are known",
                                  file.name, encoding);
            return std::nullopt;
        }
        for (const json::member &m : *o)
            if (m.key != "name" && m.key != "encoding" && m.key != "data")
                file.extra.set(m.key, m.value);
        return file;
    }

    // ---- library -------------------------------------------------------------------

    std::string_view library::display_name() const noexcept
    {
        return title.empty() ? std::string_view(name) : std::string_view(title);
    }

    const part *library::find(std::string_view id) const noexcept
    {
        for (const part &p : parts)
            if (p.id == id)
                return &p;
        return nullptr;
    }

    part *library::find(std::string_view id) noexcept
    {
        for (part &p : parts)
            if (p.id == id)
                return &p;
        return nullptr;
    }

    const embedded_file *library::find_file(std::string_view file_name) const noexcept
    {
        for (const embedded_file &f : files)
            if (f.name == file_name)
                return &f;
        return nullptr;
    }

    bool library::operator==(const library &other) const
    {
        // The content; where it was loaded from and the version of the
        // format it was read in are not.
        return name == other.name && title == other.title && description == other.description &&
               version == other.version && author == other.author && license == other.license && url == other.url &&
               parts == other.parts && files == other.files && extra == other.extra;
    }

    // ---- load report -----------------------------------------------------------------

    void load_report::add(problem p)
    {
        problems.push_back(std::move(p));
    }

    void load_report::add(std::string message)
    {
        problem p;
        p.message = std::move(message);
        problems.push_back(std::move(p));
    }

    void load_report::add(std::size_t part, std::string id, std::string message, bool skipped)
    {
        problem p;
        p.part = part;
        p.id = std::move(id);
        p.message = std::move(message);
        p.skipped = skipped;
        problems.push_back(std::move(p));
    }

    void load_report::merge(const load_report &other)
    {
        problems.insert(problems.end(), other.problems.begin(), other.problems.end());
        parts_loaded += other.parts_loaded;
        parts_skipped += other.parts_skipped;
        seconds += other.seconds;
    }

    std::string load_report::summary() const
    {
        std::string text = std::format("{} part(s) loaded, {} skipped, {} problem(s) in {:.1f} ms", parts_loaded,
                                       parts_skipped, problems.size(), seconds * 1000.0);
        for (const problem &p : problems)
        {
            text += "\n  ";
            if (!p.file.empty())
                text += utf8_name(p.file) + ": ";
            if (p.part != problem::no_part)
            {
                text += std::format("part {}", p.part);
                if (!p.id.empty())
                    text += " (" + p.id + ")";
                text += ": ";
            }
            text += p.message;
            if (p.skipped)
                text += " (left out)";
        }
        return text;
    }

    // ---- reading -----------------------------------------------------------------------

    std::expected<loaded_library, std::string> parse_library(std::string_view text, std::string_view name_hint)
    {
        const auto start = std::chrono::steady_clock::now();
        auto document = json::parse(text);
        if (!document)
            return std::unexpected(document.error().message());
        const json::object *root = document->as_object();
        if (root == nullptr)
            return std::unexpected(std::format("not a Volt library: the file holds {}, not an object",
                                               json::describe(document->type())));
        const auto format = root->find("format") ? root->find("format")->as_string() : std::nullopt;
        if (!format || *format != library_format)
            return std::unexpected(std::format("not a Volt library: \"format\" should be \"{}\"", library_format));

        loaded_library result;
        library &lib = result.library;
        load_report &report = result.report;
        const auto file_problem = [&](std::string message) {
            report.add(std::move(message));
        };

        // The format version: older files are read as they are, newer ones
        // as far as this version understands them, keeping the rest.
        if (const json::value *version = root->find("format_version"))
        {
            const auto number = version->as_integer();
            if (!number || *number < 1)
                return std::unexpected("\"format_version\" should be a whole number from 1");
            lib.format_version = static_cast<int>(std::min<std::int64_t>(*number, 1'000'000));
            if (lib.format_version > library_format_version)
                file_problem(std::format("written in format version {}, newer than this Volt's {}: fields it does "
                                         "not know are kept but not used",
                                         lib.format_version, library_format_version));
        }
        else
            file_problem(std::format("no \"format_version\"; read as version {}", library_format_version));

        std::vector<std::string> messages;
        const auto read = [&](std::string_view key, std::string &out) {
            if (const json::value *v = root->find(key))
            {
                if (const auto s = v->as_string())
                    out = std::string(*s);
                else if (!v->is_null())
                    file_problem(std::format("\"{}\" should be a string, not {}; ignored", key, json::describe(v->type())));
            }
        };
        read("name", lib.name);
        read("title", lib.title);
        read("description", lib.description);
        read("version", lib.version);
        read("author", lib.author);
        read("license", lib.license);
        read("url", lib.url);
        if (!valid_id(lib.name))
        {
            if (!valid_id(name_hint))
                return std::unexpected(lib.name.empty() ? std::string("the library has no \"name\"")
                                                        : std::format("\"{}\" cannot be a library name: it must not "
                                                                      "contain ':' or control characters",
                                                                      lib.name));
            file_problem(lib.name.empty() ? std::format("no \"name\"; named \"{}\" after the file", name_hint)
                                          : std::format("\"{}\" cannot be a library name; named \"{}\" after the file",
                                                        lib.name, name_hint));
            lib.name = std::string(name_hint);
        }

        for (const json::member &m : *root)
            if (!is_known(m.key))
                lib.extra.set(m.key, m.value);

        const json::value *parts = root->find("parts");
        if (parts == nullptr)
            file_problem("no \"parts\"; the library is empty");
        else if (!parts->is_array())
            return std::unexpected(std::format("\"parts\" should be an array, not {}", json::describe(parts->type())));
        else
        {
            const json::array &array = *parts->as_array();
            lib.parts.reserve(array.size());
            std::unordered_map<std::string, std::size_t, string_hash, std::equal_to<>> seen;
            seen.reserve(array.size());
            for (std::size_t i = 0; i < array.size(); ++i)
            {
                const auto skip = [&](std::string id, std::string message) {
                    report.add(i, std::move(id), std::move(message), true);
                    ++report.parts_skipped;
                };
                const json::object *o = array[i].as_object();
                if (o == nullptr)
                {
                    skip({}, std::format("should be an object, not {}", json::describe(array[i].type())));
                    continue;
                }
                messages.clear();
                part p = part_from_json(*o, messages);
                if (p.id.empty())
                {
                    skip({}, "has no \"id\"");
                    continue;
                }
                if (!valid_id(p.id))
                {
                    skip(p.id, "the id must not contain ':' or control characters, or start or end with a space");
                    continue;
                }
                if (const auto it = seen.find(p.id); it != seen.end())
                {
                    skip(p.id, std::format("duplicate id, first used by part {}", it->second));
                    continue;
                }
                seen.emplace(p.id, i);
                for (std::string &message : messages)
                    report.add(i, p.id, std::move(message));
                if (p.name.empty())
                {
                    report.add(i, p.id, "has no \"name\"; named after its id");
                    p.name = p.id;
                }
                lib.parts.push_back(std::move(p));
                ++report.parts_loaded;
            }
        }
        if (const json::value *files = root->find("files"))
        {
            if (const json::array *entries = files->as_array())
            {
                for (const json::value &entry : *entries)
                {
                    std::string problem;
                    auto file = file_from_json(entry, problem);
                    if (!file)
                        file_problem(problem + "; left out");
                    else if (lib.find_file(file->name) != nullptr)
                        file_problem(std::format("the file \"{}\" is there twice; the first is kept", file->name));
                    else
                        lib.files.push_back(std::move(*file));
                }
            }
            else if (!files->is_null())
                file_problem(std::format("\"files\" should be an array, not {}; ignored", json::describe(files->type())));
        }
        report.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        return result;
    }

    std::expected<loaded_library, std::string> load_library(const std::filesystem::path &path)
    {
        auto bytes = json::read_text_file(path);
        if (!bytes)
            return std::unexpected(std::move(bytes.error()));
        const std::u8string stem = path.stem().u8string();
        auto result = parse_library(*bytes, std::string_view(reinterpret_cast<const char *>(stem.data()), stem.size()));
        if (!result)
            return std::unexpected(utf8_name(path) + ": " + result.error());
        result->library.path = path;
        for (load_report::problem &p : result->report.problems)
            p.file = path;
        return result;
    }

    // ---- writing -----------------------------------------------------------------------

    json::value library_to_json(const library &lib)
    {
        json::object o;
        o.set("format", json::value(library_format));
        o.set("format_version", json::value(library_format_version));
        put(o, "name", lib.name);
        put(o, "title", lib.title);
        put(o, "description", lib.description);
        put(o, "version", lib.version);
        put(o, "author", lib.author);
        put(o, "license", lib.license);
        put(o, "url", lib.url);
        for (const json::member &m : lib.extra)
            if (!is_known(m.key))
                o.set(m.key, m.value);
        json::array parts;
        parts.reserve(lib.parts.size());
        for (const part &p : lib.parts)
            parts.push_back(part_to_json(p));
        o.set("parts", json::value(std::move(parts)));
        // Last, and only when there are any: they are long.
        if (!lib.files.empty())
        {
            json::array files;
            files.reserve(lib.files.size());
            for (const embedded_file &f : lib.files)
                files.push_back(file_to_json(f));
            o.set("files", json::value(std::move(files)));
        }
        return json::value(std::move(o));
    }

    std::string write_library(const library &lib)
    {
        return json::write(library_to_json(lib));
    }

    std::expected<void, std::string> save_library(const std::filesystem::path &path, const library &lib)
    {
        return json::write_text_file(path, write_library(lib));
    }

} // namespace volt::library
