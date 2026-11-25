#pragma once

#include "../math/check.hpp"

#include <library/catalog.hpp>
#include <library/library.hpp>
#include <library/project.hpp>

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

// Shared by the library tests: a scratch directory, and small parts and
// libraries made to order.

namespace volt::tests
{

    // A directory of its own under the system's temporary directory, empty
    // to start with and removed at the end.
    class scratch_directory
    {
    public:
        explicit scratch_directory(std::string_view name)
            : m_path(std::filesystem::temp_directory_path() / library::path_from_utf8(name))
        {
            std::error_code error;
            std::filesystem::remove_all(m_path, error);
            std::filesystem::create_directories(m_path);
        }

        ~scratch_directory()
        {
            std::error_code error;
            std::filesystem::remove_all(m_path, error);
        }

        scratch_directory(const scratch_directory &) = delete;
        scratch_directory &operator=(const scratch_directory &) = delete;

        const std::filesystem::path &path() const noexcept { return m_path; }

        // A path below it, from UTF-8.
        std::filesystem::path operator/(std::string_view relative) const
        {
            return m_path / library::path_from_utf8(relative);
        }

    private:
        std::filesystem::path m_path;
    };

    inline library::part make_part(std::string id, std::string name, std::string category, std::string value = {},
                                   std::string package = {})
    {
        library::part p;
        p.id = std::move(id);
        p.name = std::move(name);
        p.category = std::move(category);
        p.value = std::move(value);
        p.package = std::move(package);
        return p;
    }

    inline library::library make_library(std::string name, std::string version = "1.0")
    {
        library::library lib;
        lib.name = std::move(name);
        lib.version = std::move(version);
        return lib;
    }

    inline bool write_text(const std::filesystem::path &path, std::string_view text)
    {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        const auto written = json::write_text_file(path, text);
        if (!written)
            std::fprintf(stderr, "  %s\n", written.error().c_str());
        return written.has_value();
    }

} // namespace volt::tests
