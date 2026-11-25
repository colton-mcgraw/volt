// Loads every font in the given directories (default: the system's fonts),
// every face of every collection, and runs everything on every glyph; then,
// with --corrupt N, does the same to N damaged copies of each file. Reports
// what failed to load and how many glyphs failed. Not a test: the system's
// fonts are not the repository's. A crash or hang here is the bug to find.
//
//   volt_font_robustness [--corrupt N] [directory...]

#include "common.hpp"
#include "exercise.hpp"

#include <cctype>
#include <chrono>
#include <filesystem>
#include <random>

using namespace volt::font;

int main(int argc, char **argv)
{
    std::vector<std::filesystem::path> directories;
    int corrupt = 0;
    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];
        if (argument == "--corrupt" && i + 1 < argc)
            corrupt = std::atoi(argv[++i]);
        else
            directories.emplace_back(argument);
    }
    if (directories.empty())
    {
#ifdef _WIN32
        directories.emplace_back("C:/Windows/Fonts");
#else
        directories.emplace_back("/usr/share/fonts");
#endif
    }

    std::size_t files = 0;
    std::size_t faces = 0;
    std::size_t glyphs = 0;
    std::size_t glyph_errors = 0;
    std::size_t refused = 0;
    std::size_t cff_faces = 0;
    std::mt19937 random(1234);
    const auto start = std::chrono::steady_clock::now();

    for (const auto &directory : directories)
    {
        std::error_code error;
        for (const auto &entry : std::filesystem::recursive_directory_iterator(directory, error))
        {
            std::string extension = entry.path().extension().string();
            for (char &c : extension)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (extension != ".ttf" && extension != ".otf" && extension != ".ttc" && extension != ".otc")
                continue;
            ++files;

            const std::vector<std::byte> bytes = volt::tests::font::read_file(entry.path().string());
            const auto count = font::face_count(bytes);
            if (!count)
            {
                ++refused;
                std::printf("refused %s: %s\n", entry.path().filename().string().c_str(), count.error().c_str());
                continue;
            }
            for (std::uint32_t index = 0; index < *count; ++index)
            {
                const auto loaded = font::load(bytes, index);
                if (!loaded)
                {
                    ++refused;
                    std::printf("refused %s #%u: %s\n", entry.path().filename().string().c_str(), index,
                                loaded.error().c_str());
                    continue;
                }
                ++faces;
                if (loaded->format() == outline_format::cff)
                    ++cff_faces;
                const auto counts = volt::tests::font::exercise(*loaded, 31);
                glyphs += counts.glyphs;
                glyph_errors += counts.outline_errors;
                if (counts.outline_errors != 0)
                    std::printf("%s #%u (%s %s): %zu of %zu glyphs failed\n", entry.path().filename().string().c_str(),
                                index, loaded->family().c_str(), loaded->style().c_str(), counts.outline_errors,
                                counts.glyphs);
            }

            for (int round = 0; round < corrupt; ++round)
            {
                std::vector<std::byte> damaged = bytes;
                if (round % 3 == 0)
                    damaged.resize(std::uniform_int_distribution<std::size_t>(0, damaged.size())(random));
                const int flips = 1 + round % 50;
                for (int flip = 0; flip < flips && !damaged.empty(); ++flip)
                {
                    const std::size_t at = std::uniform_int_distribution<std::size_t>(0, damaged.size() - 1)(random);
                    damaged[at] ^= static_cast<std::byte>(1u << (random() % 8));
                }
                if (const auto loaded = font::load(damaged, 0); loaded)
                    (void)volt::tests::font::exercise(*loaded, 97);
            }
        }
    }

    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("%zu files, %zu faces (%zu CFF), %zu refused; %zu glyphs, %zu failed; %d corrupted copies each; %.1f s\n",
                files, faces, cff_faces, refused, glyphs, glyph_errors, corrupt, seconds);
    return 0;
}
