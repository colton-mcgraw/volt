#include <library/edit.hpp>

#include <library/project.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <random>

namespace volt::library
{

    namespace
    {
        bool is_space(char c) noexcept
        {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r';
        }

        std::string_view trim(std::string_view text) noexcept
        {
            while (!text.empty() && is_space(text.front()))
                text.remove_prefix(1);
            while (!text.empty() && is_space(text.back()))
                text.remove_suffix(1);
            return text;
        }

        // What add_part and replace_part make sure of before a part goes in.
        std::expected<void, std::string> prepare(part &p)
        {
            if (!valid_id(p.id))
                return std::unexpected(std::format("\"{}\" cannot be a part's id: it must not be empty, contain ':' "
                                                   "or control characters, or start or end with a space",
                                                   p.id));
            if (trim(p.name).empty())
                p.name = trim(p.mpn).empty() ? p.id : p.mpn;
            p.category = normalize_category(p.category);
            return {};
        }

        // "models/R.step" and 2 make "models/R-2.step".
        std::string numbered(std::string_view name, int n)
        {
            const std::size_t slash = name.rfind('/');
            const std::size_t dot = name.rfind('.');
            const bool has_extension = dot != std::string_view::npos && dot > 0 &&
                                       (slash == std::string_view::npos || dot > slash + 1);
            if (!has_extension)
                return std::format("{}-{}", name, n);
            return std::format("{}-{}{}", name.substr(0, dot), n, name.substr(dot));
        }
    } // namespace

    library new_library(std::string_view name, std::string_view title)
    {
        library lib;
        lib.name = std::string(name);
        lib.title = std::string(title);
        return lib;
    }

    // ---- ids -----------------------------------------------------------------------

    std::string generate_id(const library &lib)
    {
        // Crockford's alphabet: no i, l, o or u, so an id read aloud or
        // copied by hand is not mistaken. Sixty bits: two parts of one
        // library will not be given the same one, and the library is
        // looked in anyway.
        constexpr std::string_view alphabet = "0123456789abcdefghjkmnpqrstvwxyz";
        thread_local std::mt19937_64 random = [] {
            std::random_device device;
            std::seed_seq seed{device(), device(), device(), device()};
            return std::mt19937_64(seed);
        }();
        while (true)
        {
            std::uint64_t bits = random();
            std::string id = "p-";
            for (int i = 0; i < 12; ++i, bits >>= 5)
                id += alphabet[bits & 31];
            if (lib.find(id) == nullptr)
                return id;
        }
    }

    std::string make_id(std::string_view text)
    {
        std::string id;
        id.reserve(text.size());
        for (const char c : trim(text))
            id += c == ':' || static_cast<unsigned char>(c) < 0x20 || c == 0x7F ? '_' : c;
        return id;
    }

    // ---- parts ---------------------------------------------------------------------

    std::expected<added_part, std::string> add_part(library &lib, part p, on_conflict conflict)
    {
        if (p.id.empty())
            p.id = generate_id(lib);
        if (auto ready = prepare(p); !ready)
            return std::unexpected(std::move(ready.error()));

        part *existing = lib.find(p.id);
        if (existing == nullptr)
        {
            lib.parts.push_back(std::move(p));
            return added_part{lib.parts.back().id, add_action::added};
        }
        switch (conflict)
        {
        case on_conflict::fail:
            return std::unexpected(std::format("the library {} already has a part \"{}\"", lib.name, p.id));
        case on_conflict::skip:
            return added_part{existing->id, add_action::skipped};
        case on_conflict::replace:
            *existing = std::move(p);
            return added_part{existing->id, add_action::replaced};
        case on_conflict::rename:
            break;
        }
        // Another part, so another identity: nothing of the taken id.
        p.id = generate_id(lib);
        lib.parts.push_back(std::move(p));
        return added_part{lib.parts.back().id, add_action::added};
    }

    std::expected<void, std::string> replace_part(library &lib, std::string_view id, part p)
    {
        part *existing = lib.find(id);
        if (existing == nullptr)
            return std::unexpected(std::format("the library {} has no part \"{}\"", lib.name, id));
        if (p.id.empty())
            p.id = std::string(id);
        if (p.id != id)
            return std::unexpected(std::format("the part \"{}\" cannot become \"{}\": a part's id does not change",
                                               id, p.id));
        if (auto ready = prepare(p); !ready)
            return std::unexpected(std::move(ready.error()));
        *existing = std::move(p);
        return {};
    }

    bool remove_part(library &lib, std::string_view id)
    {
        const auto there = std::ranges::find_if(lib.parts, [&](const part &p) { return p.id == id; });
        if (there == lib.parts.end())
            return false;
        lib.parts.erase(there);
        return true;
    }

    // ---- files ---------------------------------------------------------------------

    std::string add_file(library &lib, embedded_file file)
    {
        const std::string wanted = file.name;
        for (int n = 1;; ++n)
        {
            if (n > 1)
                file.name = numbered(wanted, n);
            const embedded_file *existing = lib.find_file(file.name);
            if (existing == nullptr)
            {
                lib.files.push_back(std::move(file));
                return lib.files.back().name;
            }
            if (existing->data == file.data)
                return existing->name;
        }
    }

    std::expected<embedded_file, std::string> read_file(const std::filesystem::path &path, std::string name)
    {
        auto bytes = json::read_text_file(path);
        if (!bytes)
            return std::unexpected(std::move(bytes.error()));
        embedded_file file;
        file.name = name.empty() ? path_to_utf8(path.filename()) : std::move(name);
        if (!valid_file_name(file.name))
            return std::unexpected(std::format("\"{}\" cannot be a file's name in a library", file.name));
        file.data = std::move(*bytes);
        return file;
    }

    void carry_files(library &to, std::span<const embedded_file> from, part &p)
    {
        if (p.model.empty())
            return;
        const auto file = std::ranges::find_if(from, [&](const embedded_file &f) { return f.name == p.model; });
        if (file != from.end())
            p.model = add_file(to, *file);
    }

    std::size_t remove_unused_files(library &lib)
    {
        return static_cast<std::size_t>(std::erase_if(lib.files, [&](const embedded_file &f) {
            return std::ranges::none_of(lib.parts, [&](const part &p) { return p.model == f.name; });
        }));
    }

    std::expected<std::filesystem::path, std::string> extract_file(const embedded_file &file,
                                                                   const std::filesystem::path &directory)
    {
        if (!valid_file_name(file.name))
            return std::unexpected(std::format("\"{}\" cannot be a file's name", file.name));
        // The last segment only: the hash already tells files apart.
        const std::string_view name = std::string_view(file.name).substr(file.name.rfind('/') + 1);
        const std::filesystem::path path = directory / content_hash(file.data) / path_from_utf8(name);
        std::error_code error;
        if (std::filesystem::is_regular_file(path, error) && std::filesystem::file_size(path, error) == file.data.size())
            return path;
        std::filesystem::create_directories(path.parent_path(), error);
        if (auto written = json::write_text_file(path, file.data); !written)
            return std::unexpected(std::move(written.error()));
        return path;
    }

} // namespace volt::library
