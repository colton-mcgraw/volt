#pragma once

#include <model/model.hpp>
#include <model/options.hpp>

#include <expected>
#include <string>
#include <string_view>

// -------------------------------------------------------------------------
// stl
// -------------------------------------------------------------------------
//
// STL, binary and ASCII: a soup of triangles, nothing else. Read, the
// corners are welded by position and normals computed with hard edges at
// creases (the facet normals files carry are too often wrong to use); every
// `solid` of an ASCII file is a mesh of its own. No units, so millimeters.
// Written, the model is flattened into world coordinates as one solid.

namespace volt::model::stl
{

    std::expected<model, std::string> read(std::string_view bytes, load_report &report);

    // Whether the bytes are a binary STL: the size is the one the triangle
    // count in the header implies. (An ASCII file starts with "solid", but so
    // do many binary ones.)
    bool is_binary(std::string_view bytes) noexcept;

    std::string write_binary(const model &model);
    std::string write_ascii(const model &model);

} // namespace volt::model::stl
