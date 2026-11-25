#pragma once

#include "../math/check.hpp"

#include <font/font.hpp>

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// Shared by the font tests: the repository's two fonts, read once.
//
// DefaultFont.ttf is Arial 2048 upem, TrueType with a 'kern' table and the
// same kerning in GPOS. SairaStencil.ttf is the default (Thin) instance of
// a variable font, 1000 upem, kerning only in GPOS (pair formats 1 and 2),
// with USE_TYPO_METRICS set. The known answers in the tests were read with
// fontTools, independently of this code.

#ifndef VOLT_ASSETS_DIR
#error "VOLT_ASSETS_DIR must point at the repository's assets directory"
#endif

namespace volt::tests::font
{
    inline std::vector<std::byte> read_file(const std::string &path)
    {
        std::ifstream stream(path, std::ios::binary);
        std::vector<char> chars((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        std::vector<std::byte> bytes(chars.size());
        for (std::size_t i = 0; i < chars.size(); ++i)
            bytes[i] = static_cast<std::byte>(chars[i]);
        return bytes;
    }

    inline std::string asset(const char *name)
    {
        return std::string(VOLT_ASSETS_DIR) + "/fonts/" + name;
    }

    // Loads or fails the whole test run: nothing after would mean anything.
    inline volt::font::font load(const char *name)
    {
        auto loaded = volt::font::font::load_file(asset(name));
        if (!loaded)
        {
            std::fprintf(stderr, "cannot load %s: %s\n", name, loaded.error().c_str());
            std::exit(1);
        }
        return *loaded;
    }

    inline volt::font::font arial()
    {
        static const volt::font::font font = load("DefaultFont.ttf");
        return font;
    }

    inline volt::font::font saira()
    {
        static const volt::font::font font = load("SairaStencil.ttf");
        return font;
    }

    inline bool near(float a, float b, float tolerance)
    {
        return std::abs(a - b) <= tolerance;
    }
} // namespace volt::tests::font
