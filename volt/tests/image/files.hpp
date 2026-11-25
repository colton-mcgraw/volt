#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// Files for the image tests: the pictures in assets/images (VOLT_ASSETS_DIR,
// set by tests.cmake), and a directory in the build tree where tests leave
// what they rendered, for a person to look at (VOLT_TEST_OUTPUT_DIR).

namespace volt::tests
{
    inline std::filesystem::path asset(const char *name)
    {
        return std::filesystem::path(VOLT_ASSETS_DIR) / "images" / name;
    }

    inline std::vector<std::uint8_t> read_bytes(const std::filesystem::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    inline std::filesystem::path output(const char *name)
    {
        const std::filesystem::path directory(VOLT_TEST_OUTPUT_DIR);
        std::error_code ignored;
        std::filesystem::create_directories(directory, ignored);
        return directory / name;
    }

    inline void write_bytes(const std::filesystem::path &path, const std::vector<std::uint8_t> &bytes)
    {
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    // A deterministic generator for test data.
    struct xorshift
    {
        std::uint64_t state = 0x9E3779B97F4A7C15ull;

        std::uint32_t next()
        {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            return static_cast<std::uint32_t>(state >> 16);
        }
    };
} // namespace volt::tests
