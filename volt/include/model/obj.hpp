#pragma once

#include <model/model.hpp>
#include <model/options.hpp>

#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

// -------------------------------------------------------------------------
// obj
// -------------------------------------------------------------------------
//
// Wavefront OBJ, with its MTL materials: the plainest format for meshes.
//
// Read: positions (with the common `v x y z r g b` vertex colors), normals,
// polygons (fanned into triangles; negative, relative indices included),
// objects and groups (o, g) and materials (usemtl) which split the meshes,
// and from the MTL the diffuse color (Kd) and opacity (d, Tr). Texture
// coordinates and everything else are skipped. Faces without normals get
// them computed, with hard edges at creases. OBJ has no units: values are
// taken as millimeters.
//
// Written: the model flattened into world coordinates, a mesh per `o`, a
// material per mesh in the MTL next to it, colors converted to sRGB.

namespace volt::model::obj
{

    // Where the MTL files an OBJ names (`mtllib name.mtl`) come from: given
    // the name, their text, or nothing if there is no such file.
    using material_source = std::function<std::optional<std::string>(std::string_view name)>;

    std::expected<model, std::string> read(std::string_view text, const material_source &materials,
                                           load_report &report);

    struct written
    {
        std::string obj;
        std::string mtl;
    };

    // `mtl_name` is what the OBJ's mtllib line names, the file the MTL text
    // is to be saved as.
    written write(const model &model, std::string_view mtl_name);

} // namespace volt::model::obj
