// Writes the seed libraries into a directory:
//
//   volt_library_seed <directory>
//
// or through the build: `cmake --build <build> --target volt_generate_libraries`.

#include "seed.hpp"

#include <library/project.hpp>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace
{
    // The command line in UTF-8, which is what the directory is read as.
    int run(int argc, char **argv);
} // namespace

#ifdef _WIN32
// On Windows main()'s arguments are in the system's ANSI code page, which
// cannot hold every file name and is not UTF-8; the wide ones are converted
// instead.
int wmain(int argc, wchar_t **wide)
{
    std::vector<std::string> arguments_utf8;
    for (int i = 0; i < argc; ++i)
    {
        const int size = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
        std::string text(static_cast<std::size_t>(size > 0 ? size - 1 : 0), char(0));
        if (size > 1)
            WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, text.data(), size, nullptr, nullptr);
        arguments_utf8.push_back(std::move(text));
    }
    std::vector<char *> argv;
    for (std::string &text : arguments_utf8)
        argv.push_back(text.data());
    argv.push_back(nullptr);
    // Messages name files in UTF-8 too.
    SetConsoleOutputCP(CP_UTF8);
    return run(argc, argv.data());
}
#else
int main(int argc, char **argv)
{
    return run(argc, argv);
}
#endif

namespace
{
int run(int argc, char **argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: volt_library_seed <directory>\n");
        return 2;
    }
    const std::filesystem::path directory = volt::library::path_from_utf8(argv[1]);
    std::error_code error;
    std::filesystem::create_directories(directory, error);

    std::size_t total = 0;
    for (const volt::library::library &lib : volt::library::seed::libraries())
    {
        const std::filesystem::path path = directory / volt::library::path_from_utf8(lib.name + ".voltlib");
        if (auto saved = volt::library::save_library(path, lib); !saved)
        {
            std::fprintf(stderr, "%s\n", saved.error().c_str());
            return 1;
        }
        std::printf("%-22s %5zu parts\n", lib.name.c_str(), lib.parts.size());
        total += lib.parts.size();
    }
    std::printf("%-22s %5zu parts\n", "total", total);
    return 0;
}
} // namespace
