#include <model/stl.hpp>

#include <math/geometry.hpp>

#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <format>

namespace volt::model::stl
{

    namespace
    {
        std::uint32_t read_u32(const char *p)
        {
            std::uint32_t v = 0;
            std::memcpy(&v, p, 4);
            if constexpr (std::endian::native == std::endian::big)
                v = std::byteswap(v);
            return v;
        }

        float read_f32(const char *p)
        {
            return std::bit_cast<float>(read_u32(p));
        }

        void put_u32(std::string &out, std::uint32_t v)
        {
            if constexpr (std::endian::native == std::endian::big)
                v = std::byteswap(v);
            char bytes[4];
            std::memcpy(bytes, &v, 4);
            out.append(bytes, 4);
        }

        void put_f32(std::string &out, float v)
        {
            put_u32(out, std::bit_cast<std::uint32_t>(v));
        }

        // A triangle soup into a mesh: the corners are taken as they are
        // (one vertex each), and compute_normals() welds them.
        mesh finish(std::string name, std::vector<math::vec3f> &corners)
        {
            mesh m;
            m.name = std::move(name);
            m.vertices.reserve(corners.size());
            for (const math::vec3f &p : corners)
                m.vertices.push_back({p, {}});
            m.indices.resize(corners.size());
            for (std::size_t i = 0; i < corners.size(); ++i)
                m.indices[i] = static_cast<std::uint32_t>(i);
            compute_normals(m);
            return m;
        }

        std::expected<model, std::string> read_binary(std::string_view bytes, load_report &report)
        {
            const std::uint32_t count = read_u32(bytes.data() + 80);
            std::vector<math::vec3f> corners;
            corners.reserve(static_cast<std::size_t>(count) * 3);
            for (std::uint32_t t = 0; t < count; ++t)
            {
                const char *record = bytes.data() + 84 + static_cast<std::size_t>(t) * 50;
                math::vec3f triangle[3];
                bool finite = true;
                for (int k = 0; k < 3; ++k)
                {
                    for (int c = 0; c < 3; ++c)
                    {
                        const float v = read_f32(record + 12 + 12 * k + 4 * c);
                        finite = finite && std::isfinite(v);
                        triangle[k][static_cast<std::size_t>(c)] = v;
                    }
                }
                if (!finite)
                {
                    report.warn(std::format("triangle {} has coordinates that are not numbers", t));
                    continue;
                }
                corners.insert(corners.end(), triangle, triangle + 3);
            }
            report.faces = corners.size() / 3;

            // The header is free text, often the exporter's name.
            std::string name(bytes.substr(0, 80));
            while (!name.empty() && (name.back() == '\0' || name.back() == ' '))
                name.pop_back();
            if (name.find('\0') != std::string::npos)
                name.clear();

            model result;
            result.meshes.push_back(finish(name, corners));
            result.nodes.push_back({});
            result.nodes[0].meshes.push_back(0);
            report.solids = 1;
            if (result.meshes[0].indices.empty())
                return std::unexpected("the file has no triangles");
            return result;
        }

        std::expected<model, std::string> read_ascii(std::string_view text, load_report &report)
        {
            model result;
            result.nodes.push_back({});
            std::vector<math::vec3f> corners;
            std::string name;
            std::size_t in_facet = 0;
            std::size_t line = 1;

            std::size_t i = 0;
            const auto word = [&]() -> std::string_view {
                while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])))
                {
                    if (text[i] == '\n')
                        ++line;
                    ++i;
                }
                const std::size_t start = i;
                while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i])))
                    ++i;
                return text.substr(start, i - start);
            };
            const auto rest_of_line = [&]() {
                const std::size_t start = i;
                while (i < text.size() && text[i] != '\n')
                    ++i;
                std::string_view r = text.substr(start, i - start);
                while (!r.empty() && std::isspace(static_cast<unsigned char>(r.front())))
                    r.remove_prefix(1);
                while (!r.empty() && std::isspace(static_cast<unsigned char>(r.back())))
                    r.remove_suffix(1);
                return std::string(r);
            };
            const auto close_solid = [&] {
                if (!corners.empty())
                {
                    result.nodes[0].meshes.push_back(static_cast<std::uint32_t>(result.meshes.size()));
                    result.meshes.push_back(finish(name, corners));
                    ++report.solids;
                }
                corners.clear();
            };

            for (;;)
            {
                const std::string_view w = word();
                if (w.empty())
                    break;
                if (w == "solid")
                {
                    close_solid();
                    name = rest_of_line();
                }
                else if (w == "endsolid")
                {
                    rest_of_line();
                    close_solid();
                }
                else if (w == "facet")
                {
                    in_facet = 0;
                    rest_of_line(); // the normal, recomputed anyway
                }
                else if (w == "vertex")
                {
                    math::vec3f p{};
                    for (std::size_t c = 0; c < 3; ++c)
                    {
                        std::string_view n = word();
                        if (!n.empty() && n.front() == '+')
                            n.remove_prefix(1);
                        float value = 0.0f;
                        const auto [end, error] = std::from_chars(n.data(), n.data() + n.size(), value);
                        if (error != std::errc{} || end != n.data() + n.size() || !std::isfinite(value))
                            return std::unexpected(std::format("line {}: a vertex needs three numbers", line));
                        p[c] = value;
                    }
                    if (in_facet < 3)
                        corners.push_back(p);
                    ++in_facet;
                }
                else if (w == "endfacet")
                {
                    if (in_facet != 3)
                    {
                        // Not a triangle: drop what was read of it.
                        corners.resize(corners.size() - std::min<std::size_t>(in_facet, 3));
                        report.warn(std::format("line {}: a facet with {} corners", line, in_facet));
                    }
                    else
                        ++report.faces;
                    in_facet = 0;
                }
                else if (w == "outer" || w == "loop" || w == "endloop")
                    continue;
                else
                    return std::unexpected(std::format("line {}: unexpected '{}'", line, w));
            }
            close_solid();
            if (result.meshes.empty())
                return std::unexpected("the file has no triangles");
            return result;
        }

        template <typename Emit>
        void for_each_triangle(const model &source, Emit &&emit)
        {
            const model flat = flatten(source);
            for (const mesh &m : flat.meshes)
            {
                for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3)
                {
                    const math::vec3f &a = m.vertices[m.indices[t]].position;
                    const math::vec3f &b = m.vertices[m.indices[t + 1]].position;
                    const math::vec3f &c = m.vertices[m.indices[t + 2]].position;
                    math::vec3d n = math::cross((b - a).cast<double>(), (c - a).cast<double>());
                    const double length = math::magnitude(n);
                    n = length > 0.0 ? n / length : math::vec3d{};
                    emit(n.cast<float>(), a, b, c);
                }
            }
        }
    } // namespace

    bool is_binary(std::string_view bytes) noexcept
    {
        if (bytes.size() < 84)
            return false;
        const std::uint64_t count = read_u32(bytes.data() + 80);
        return 84 + count * 50 == bytes.size();
    }

    std::expected<model, std::string> read(std::string_view bytes, load_report &report)
    {
        if (is_binary(bytes))
            return read_binary(bytes, report);
        std::size_t start = 0;
        while (start < bytes.size() && std::isspace(static_cast<unsigned char>(bytes[start])))
            ++start;
        if (bytes.substr(start, 5) == "solid")
            return read_ascii(bytes, report);
        if (bytes.size() >= 84)
            return std::unexpected("not an STL file: the size does not match the triangle count");
        return std::unexpected("not an STL file");
    }

    std::string write_binary(const model &model)
    {
        std::string out(80, '\0');
        const std::string_view header = "Binary STL written by Volt, millimeters";
        out.replace(0, header.size(), header);
        put_u32(out, 0);
        std::uint32_t count = 0;
        for_each_triangle(model, [&](const math::vec3f &n, const math::vec3f &a, const math::vec3f &b,
                                     const math::vec3f &c) {
            for (const math::vec3f *v : {&n, &a, &b, &c})
                for (std::size_t k = 0; k < 3; ++k)
                    put_f32(out, (*v)[k]);
            out.append(2, '\0');
            ++count;
        });
        std::uint32_t stored = count;
        if constexpr (std::endian::native == std::endian::big)
            stored = std::byteswap(stored);
        std::memcpy(out.data() + 80, &stored, 4);
        return out;
    }

    std::string write_ascii(const model &model)
    {
        std::string name = model.name.empty() ? std::string("volt") : model.name;
        for (char &c : name)
            if (std::isspace(static_cast<unsigned char>(c)))
                c = '_';
        std::string out = std::format("solid {}\n", name);
        for_each_triangle(model, [&](const math::vec3f &n, const math::vec3f &a, const math::vec3f &b,
                                     const math::vec3f &c) {
            out += std::format("  facet normal {} {} {}\n    outer loop\n", n[0], n[1], n[2]);
            for (const math::vec3f *v : {&a, &b, &c})
                out += std::format("      vertex {} {} {}\n", (*v)[0], (*v)[1], (*v)[2]);
            out += "    endloop\n  endfacet\n";
        });
        out += std::format("endsolid {}\n", name);
        return out;
    }

} // namespace volt::model::stl
