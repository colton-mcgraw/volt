#pragma once

#include <model/model.hpp>
#include <model/options.hpp>
#include <model/step.hpp>

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

// -------------------------------------------------------------------------
// io
// -------------------------------------------------------------------------
//
// The front door: a model file in, whatever its format, and a model out to
// the format a file name asks for. The formats themselves live in
// <model/step.hpp>, <model/obj.hpp> and <model/stl.hpp>.

namespace volt::model
{

    enum class format
    {
        unknown,
        step, // .step .stp, ISO 10303-21
        obj,  // .obj, with its .mtl
        stl,  // .stl, binary or ASCII
    };

    // What the bytes are, from their content first (a STEP file starts with
    // ISO-10303-21, a binary STL has the size its header implies, ...), from
    // the extension (with or without the dot, any case) where the content
    // does not tell.
    format detect(std::string_view bytes, std::string_view extension = {});

    // The format a file name asks for by its extension.
    format format_of(const std::filesystem::path &path);

    struct loaded
    {
        model model;
        load_report report;
    };

    std::expected<loaded, std::string> load(const std::filesystem::path &path, const load_options &options = {});

    // A file already in memory. An OBJ read this way has no MTL files to
    // find, and so no materials.
    std::expected<loaded, std::string> load(std::string_view bytes, format format, const load_options &options = {});

    struct save_options
    {
        step::write_options step;

        // STL as text rather than binary.
        bool ascii_stl = false;
    };

    // Writes the model in the format the path's extension names. An OBJ is
    // accompanied by its MTL, written next to it with the same stem.
    std::expected<void, std::string> save(const std::filesystem::path &path, const model &model,
                                          const save_options &options = {});

} // namespace volt::model
