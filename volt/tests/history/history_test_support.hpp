#pragma once

#include "../math/check.hpp"

#include <history/repository.hpp>

#include <json/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

// Shared by the tests of snapshots on disk: a scratch directory, files
// written and read by UTF-8 path, and a clock that does not move.

namespace volt::tests
{

    inline std::filesystem::path utf8_path(std::string_view text)
    {
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    }

    // A directory of its own under the system's temporary directory, empty
    // to start with and removed at the end.
    class scratch_directory
    {
    public:
        explicit scratch_directory(std::string_view name)
            : m_path(std::filesystem::temp_directory_path() / utf8_path(name))
        {
            std::error_code error;
            std::filesystem::remove_all(m_path, error);
            std::filesystem::create_directories(m_path, error);
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
        std::filesystem::path operator/(std::string_view relative) const { return m_path / utf8_path(relative); }

    private:
        std::filesystem::path m_path;
    };

    inline bool write_text(const std::filesystem::path &path, std::string_view text)
    {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        const auto written = json::write_text_file(path, text);
        if (!written)
            std::fprintf(stderr, "  %s\n", written.error().c_str());
        return written.has_value();
    }

    // The file's bytes, or "<missing>".
    inline std::string read_text(const std::filesystem::path &path)
    {
        auto bytes = json::read_text_file(path);
        return bytes ? *bytes : std::string("<missing>");
    }

    inline bool file_exists(const std::filesystem::path &path)
    {
        std::error_code error;
        return std::filesystem::exists(path, error);
    }

    // The regular files below a directory, at any depth.
    inline std::size_t count_files(const std::filesystem::path &directory)
    {
        std::size_t count = 0;
        std::error_code error;
        for (std::filesystem::recursive_directory_iterator it(directory, error), end; !error && it != end;
             it.increment(error))
            if (it->is_regular_file(error))
                ++count;
        return count;
    }

    // Where the repository keeps an object.
    inline std::filesystem::path object_path(const history::repository &repo, std::string_view kind,
                                             std::string_view id)
    {
        return repo.directory() / utf8_path(kind) / utf8_path(id.substr(0, 2)) / utf8_path(id.substr(2));
    }

    // 2026-11-22T14:03:07Z.
    inline std::chrono::sys_seconds fixed_time()
    {
        using namespace std::chrono;
        return sys_days{year{2026} / November / 22} + hours{14} + minutes{3} + seconds{7};
    }

    inline history::clock_function fixed_clock()
    {
        return [] { return fixed_time(); };
    }

    inline bool contains(std::string_view text, std::string_view part)
    {
        return text.find(part) != std::string_view::npos;
    }

    // Prints the error of a failed expected, for a CHECK that is about to fail.
    template <typename E>
    bool ok(const E &result)
    {
        if (!result)
            std::fprintf(stderr, "  unexpected error: %s\n", result.error().c_str());
        return result.has_value();
    }

} // namespace volt::tests
