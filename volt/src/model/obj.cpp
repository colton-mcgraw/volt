#include <model/obj.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <unordered_map>
#include <vector>

namespace volt::model::obj
{

    namespace
    {
        bool is_space(char c)
        {
            return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v';
        }

        // The whitespace separated words of a line.
        std::vector<std::string_view> split(std::string_view line)
        {
            std::vector<std::string_view> words;
            std::size_t i = 0;
            while (i < line.size())
            {
                while (i < line.size() && is_space(line[i]))
                    ++i;
                const std::size_t start = i;
                while (i < line.size() && !is_space(line[i]))
                    ++i;
                if (i > start)
                    words.push_back(line.substr(start, i - start));
            }
            return words;
        }

        bool number(std::string_view text, double &value)
        {
            if (!text.empty() && text.front() == '+')
                text.remove_prefix(1);
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            return error == std::errc{} && end == text.data() + text.size() && std::isfinite(value);
        }

        bool integer(std::string_view text, std::int64_t &value)
        {
            if (!text.empty() && text.front() == '+')
                text.remove_prefix(1);
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            return error == std::errc{} && end == text.data() + text.size();
        }

        // Calls `line` for every line, joining those continued with a
        // trailing backslash, without comments.
        template <typename Line>
        void for_each_line(std::string_view text, Line &&line)
        {
            std::string joined;
            std::size_t number_of_line = 0;
            std::size_t start = 0;
            while (start <= text.size())
            {
                std::size_t end = text.find('\n', start);
                if (end == std::string_view::npos)
                    end = text.size();
                std::string_view piece = text.substr(start, end - start);
                ++number_of_line;
                if (const std::size_t hash = piece.find('#'); hash != std::string_view::npos)
                    piece = piece.substr(0, hash);
                while (!piece.empty() && is_space(piece.back()))
                    piece.remove_suffix(1);
                if (!piece.empty() && piece.back() == '\\')
                {
                    joined.append(piece.substr(0, piece.size() - 1));
                    joined.push_back(' ');
                }
                else
                {
                    joined.append(piece);
                    line(std::string_view(joined), number_of_line);
                    joined.clear();
                }
                if (end == text.size())
                    break;
                start = end + 1;
            }
        }

        std::unordered_map<std::string, material> read_materials(std::string_view text)
        {
            std::unordered_map<std::string, material> materials;
            material *current = nullptr;
            for_each_line(text, [&](std::string_view line, std::size_t) {
                const std::vector<std::string_view> w = split(line);
                if (w.empty())
                    return;
                if (w[0] == "newmtl" && w.size() >= 2)
                    current = &materials[std::string(w[1])];
                else if (current == nullptr)
                    return;
                else if (w[0] == "Kd" && w.size() >= 4)
                {
                    double c[3];
                    if (number(w[1], c[0]) && number(w[2], c[1]) && number(w[3], c[2]))
                    {
                        current->color = srgb_to_linear(math::vec3f{static_cast<float>(std::clamp(c[0], 0.0, 1.0)),
                                                                    static_cast<float>(std::clamp(c[1], 0.0, 1.0)),
                                                                    static_cast<float>(std::clamp(c[2], 0.0, 1.0))});
                    }
                }
                else if (w[0] == "d" && w.size() >= 2)
                {
                    double d = 1.0;
                    if (number(w[w.size() - 1], d))
                        current->opacity = static_cast<float>(std::clamp(d, 0.0, 1.0));
                }
                else if (w[0] == "Tr" && w.size() >= 2)
                {
                    double t = 0.0;
                    if (number(w[1], t))
                        current->opacity = static_cast<float>(std::clamp(1.0 - t, 0.0, 1.0));
                }
            });
            return materials;
        }

        struct mesh_in_progress
        {
            mesh mesh;
            std::unordered_map<std::uint64_t, std::uint32_t> vertices; // (position, normal) -> vertex
            bool needs_normals = false;
            std::string material;
        };
    } // namespace

    std::expected<model, std::string> read(std::string_view text, const material_source &materials,
                                           load_report &report)
    {
        std::vector<math::vec3f> positions;
        std::vector<math::vec3f> colors; // sRGB, where `v` lines have them
        std::vector<math::vec3f> normals;
        bool any_color = false;

        std::unordered_map<std::string, material> library;
        std::vector<mesh_in_progress> done;
        mesh_in_progress current;
        std::string object;
        std::string error;

        const auto close = [&] {
            if (!current.mesh.indices.empty())
                done.push_back(std::move(current));
            current = {};
        };

        for_each_line(text, [&](std::string_view line, std::size_t line_number) {
            if (!error.empty())
                return;
            const std::vector<std::string_view> w = split(line);
            if (w.empty())
                return;
            const std::string_view key = w[0];

            if (key == "v")
            {
                double p[3] = {};
                if (w.size() < 4 || !number(w[1], p[0]) || !number(w[2], p[1]) || !number(w[3], p[2]))
                {
                    error = std::format("line {}: a vertex needs three numbers", line_number);
                    return;
                }
                positions.push_back({static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])});
                double c[3] = {1.0, 1.0, 1.0};
                if (w.size() >= 7 && number(w[4], c[0]) && number(w[5], c[1]) && number(w[6], c[2]))
                    any_color = true;
                colors.push_back({static_cast<float>(c[0]), static_cast<float>(c[1]), static_cast<float>(c[2])});
            }
            else if (key == "vn")
            {
                double n[3] = {};
                if (w.size() < 4 || !number(w[1], n[0]) || !number(w[2], n[1]) || !number(w[3], n[2]))
                {
                    error = std::format("line {}: a normal needs three numbers", line_number);
                    return;
                }
                const double length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (length > 0.0)
                    normals.push_back({static_cast<float>(n[0] / length), static_cast<float>(n[1] / length),
                                       static_cast<float>(n[2] / length)});
                else
                    normals.push_back({0.0f, 0.0f, 0.0f});
            }
            else if (key == "f")
            {
                // v, v/vt, v//vn or v/vt/vn; negative counts back.
                std::vector<std::uint32_t> polygon;
                for (std::size_t i = 1; i < w.size(); ++i)
                {
                    const std::string_view corner = w[i];
                    const std::size_t first_slash = corner.find('/');
                    std::int64_t v = 0;
                    std::int64_t vn = 0;
                    if (!integer(corner.substr(0, first_slash), v))
                    {
                        error = std::format("line {}: a face corner must start with a vertex number", line_number);
                        return;
                    }
                    if (first_slash != std::string_view::npos)
                    {
                        const std::size_t second_slash = corner.find('/', first_slash + 1);
                        if (second_slash != std::string_view::npos && second_slash + 1 < corner.size())
                            integer(corner.substr(second_slash + 1), vn);
                    }
                    if (v < 0)
                        v += static_cast<std::int64_t>(positions.size()) + 1;
                    if (vn < 0)
                        vn += static_cast<std::int64_t>(normals.size()) + 1;
                    if (v < 1 || static_cast<std::size_t>(v) > positions.size())
                    {
                        error = std::format("line {}: vertex {} does not exist", line_number, v);
                        return;
                    }
                    if (vn < 1 || static_cast<std::size_t>(vn) > normals.size())
                    {
                        vn = 0;
                        current.needs_normals = true;
                    }

                    const std::uint64_t k = static_cast<std::uint64_t>(v) | (static_cast<std::uint64_t>(vn) << 32);
                    auto found = current.vertices.find(k);
                    if (found == current.vertices.end())
                    {
                        const auto index = static_cast<std::uint32_t>(current.mesh.vertices.size());
                        vertex made;
                        made.position = positions[static_cast<std::size_t>(v - 1)];
                        made.normal = vn > 0 ? normals[static_cast<std::size_t>(vn - 1)] : math::vec3f{};
                        current.mesh.vertices.push_back(made);
                        current.mesh.colors.push_back(colors[static_cast<std::size_t>(v - 1)]);
                        found = current.vertices.emplace(k, index).first;
                    }
                    polygon.push_back(found->second);
                }
                if (polygon.size() < 3)
                {
                    report.warn(std::format("line {}: a face with fewer than three corners", line_number));
                    return;
                }
                for (std::size_t i = 1; i + 1 < polygon.size(); ++i)
                {
                    current.mesh.indices.push_back(polygon[0]);
                    current.mesh.indices.push_back(polygon[i]);
                    current.mesh.indices.push_back(polygon[i + 1]);
                }
                ++report.faces;
            }
            else if (key == "o" || key == "g")
            {
                close();
                std::string name;
                for (std::size_t i = 1; i < w.size(); ++i)
                    name += (i > 1 ? " " : "") + std::string(w[i]);
                if (key == "o")
                    object = name;
                current.mesh.name = name.empty() ? object : name;
            }
            else if (key == "usemtl")
            {
                const std::string name = w.size() >= 2 ? std::string(w[1]) : std::string();
                if (name != current.material)
                {
                    const std::string mesh_name = current.mesh.name;
                    close();
                    current.mesh.name = mesh_name;
                }
                current.material = name;
            }
            else if (key == "mtllib")
            {
                for (std::size_t i = 1; i < w.size(); ++i)
                {
                    const std::optional<std::string> mtl = materials ? materials(w[i]) : std::nullopt;
                    if (!mtl)
                    {
                        report.warn(std::format("material library {} not found", w[i]));
                        continue;
                    }
                    for (auto &[name, m] : read_materials(*mtl))
                        library[name] = m;
                }
            }
            // vt, s, l, p and the rest: nothing a mesh needs.
        });
        close();

        if (!error.empty())
            return std::unexpected(error);

        model result;
        node root;
        for (mesh_in_progress &built : done)
        {
            mesh &m = built.mesh;
            if (const auto it = library.find(built.material); it != library.end())
                m.material = it->second;
            if (any_color)
            {
                for (math::vec3f &c : m.colors)
                    c = srgb_to_linear(c);
            }
            else
                m.colors.clear();
            if (built.needs_normals)
                compute_normals(m);
            root.meshes.push_back(static_cast<std::uint32_t>(result.meshes.size()));
            result.meshes.push_back(std::move(m));
            ++report.solids;
        }
        result.nodes.push_back(std::move(root));
        if (result.meshes.empty())
            return std::unexpected("the file has no faces");
        return result;
    }

    namespace
    {
        std::string name_of(std::string_view name, std::size_t index)
        {
            std::string result;
            for (char c : name)
                result.push_back(is_space(c) || c == '\n' ? '_' : c);
            return result.empty() ? std::format("mesh_{}", index) : result;
        }
    } // namespace

    written write(const model &source, std::string_view mtl_name)
    {
        const model flat = flatten(source);
        written out;
        out.obj = "# Written by Volt. Lengths in millimeters.\n";
        out.obj += std::format("mtllib {}\n", mtl_name);
        out.mtl = "# Written by Volt. Colors in sRGB.\n";

        std::size_t base = 1;
        for (std::size_t i = 0; i < flat.meshes.size(); ++i)
        {
            const mesh &m = flat.meshes[i];
            const std::string material_name = std::format("material_{}", i);
            const math::vec3f kd = linear_to_srgb(m.material.color);
            out.mtl += std::format("newmtl {}\nKd {} {} {}\nd {}\n\n", material_name, kd[0], kd[1], kd[2],
                                   m.material.opacity);

            out.obj += std::format("o {}\nusemtl {}\n", name_of(m.name, i), material_name);
            const bool per_vertex = m.colors.size() == m.vertices.size();
            for (std::size_t v = 0; v < m.vertices.size(); ++v)
            {
                const math::vec3f &p = m.vertices[v].position;
                if (per_vertex)
                {
                    const math::vec3f c = linear_to_srgb(m.colors[v]);
                    out.obj += std::format("v {} {} {} {} {} {}\n", p[0], p[1], p[2], c[0], c[1], c[2]);
                }
                else
                    out.obj += std::format("v {} {} {}\n", p[0], p[1], p[2]);
            }
            for (const vertex &v : m.vertices)
                out.obj += std::format("vn {} {} {}\n", v.normal[0], v.normal[1], v.normal[2]);
            for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3)
            {
                const std::size_t a = base + m.indices[t];
                const std::size_t b = base + m.indices[t + 1];
                const std::size_t c = base + m.indices[t + 2];
                out.obj += std::format("f {}//{} {}//{} {}//{}\n", a, a, b, b, c, c);
            }
            base += m.vertices.size();
        }
        return out;
    }

} // namespace volt::model::obj
