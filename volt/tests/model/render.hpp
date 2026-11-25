#pragma once

#include <model/model.hpp>

#include <math/geometry.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

// A software rasterizer for looking at models without a GPU: orthographic,
// z-buffered, Lambert shading with the vertex normals, and a minimal PNG
// writer (stored deflate blocks, no compression). For tests and tools only.

namespace volt::tests
{

    struct image
    {
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> rgb;
    };

    namespace detail
    {
        inline std::uint32_t crc32(const std::uint8_t *data, std::size_t size, std::uint32_t crc = 0)
        {
            static const auto table = [] {
                std::array<std::uint32_t, 256> t{};
                for (std::uint32_t n = 0; n < 256; ++n)
                {
                    std::uint32_t c = n;
                    for (int k = 0; k < 8; ++k)
                        c = (c & 1u) != 0 ? 0xedb88320u ^ (c >> 1) : c >> 1;
                    t[n] = c;
                }
                return t;
            }();
            crc = ~crc;
            for (std::size_t i = 0; i < size; ++i)
                crc = table[(crc ^ data[i]) & 0xffu] ^ (crc >> 8);
            return ~crc;
        }

        inline void put32(std::vector<std::uint8_t> &out, std::uint32_t v)
        {
            out.push_back(static_cast<std::uint8_t>(v >> 24));
            out.push_back(static_cast<std::uint8_t>(v >> 16));
            out.push_back(static_cast<std::uint8_t>(v >> 8));
            out.push_back(static_cast<std::uint8_t>(v));
        }

        inline void chunk(std::vector<std::uint8_t> &out, const char *type, const std::vector<std::uint8_t> &data)
        {
            put32(out, static_cast<std::uint32_t>(data.size()));
            std::vector<std::uint8_t> body(type, type + 4);
            body.insert(body.end(), data.begin(), data.end());
            out.insert(out.end(), body.begin(), body.end());
            put32(out, crc32(body.data(), body.size()));
        }
    } // namespace detail

    inline bool write_png(const std::string &path, const image &img)
    {
        std::vector<std::uint8_t> raw;
        raw.reserve(static_cast<std::size_t>(img.height) * (static_cast<std::size_t>(img.width) * 3 + 1));
        for (int y = 0; y < img.height; ++y)
        {
            raw.push_back(0); // no filter
            const auto row = static_cast<std::size_t>(y) * static_cast<std::size_t>(img.width) * 3;
            raw.insert(raw.end(), img.rgb.begin() + static_cast<std::ptrdiff_t>(row),
                       img.rgb.begin() + static_cast<std::ptrdiff_t>(row + static_cast<std::size_t>(img.width) * 3));
        }

        // zlib stream of stored deflate blocks.
        std::vector<std::uint8_t> z{0x78, 0x01};
        std::size_t at = 0;
        do
        {
            const std::size_t length = std::min<std::size_t>(65535, raw.size() - at);
            const bool last = at + length == raw.size();
            z.push_back(last ? 1 : 0);
            z.push_back(static_cast<std::uint8_t>(length & 0xff));
            z.push_back(static_cast<std::uint8_t>(length >> 8));
            z.push_back(static_cast<std::uint8_t>(~length & 0xff));
            z.push_back(static_cast<std::uint8_t>((~length >> 8) & 0xff));
            z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(at),
                     raw.begin() + static_cast<std::ptrdiff_t>(at + length));
            at += length;
        } while (at < raw.size());
        std::uint32_t a = 1;
        std::uint32_t b = 0;
        for (std::uint8_t byte : raw)
        {
            a = (a + byte) % 65521u;
            b = (b + a) % 65521u;
        }
        detail::put32(z, (b << 16) | a);

        std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
        std::vector<std::uint8_t> header;
        detail::put32(header, static_cast<std::uint32_t>(img.width));
        detail::put32(header, static_cast<std::uint32_t>(img.height));
        header.insert(header.end(), {8, 2, 0, 0, 0}); // 8-bit RGB
        detail::chunk(png, "IHDR", header);
        detail::chunk(png, "IDAT", z);
        detail::chunk(png, "IEND", {});

        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char *>(png.data()), static_cast<std::streamsize>(png.size()));
        return static_cast<bool>(out);
    }

    struct view
    {
        math::vec3d direction{-1.0, -1.2, 0.9}; // from the eye towards the model, reversed: where the eye is
        math::vec3d light{-0.4, -0.6, 1.0};
        bool wireframe = false;
    };

    // Draws every placed mesh into `img`, which it sizes. The model fills
    // the frame.
    inline void render(const model::model &source, image &img, int width, int height, const view &v)
    {
        img.width = width;
        img.height = height;
        img.rgb.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3, 0);
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                const auto i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 3;
                const auto shade = static_cast<std::uint8_t>(40 + 30 * y / height);
                img.rgb[i] = shade;
                img.rgb[i + 1] = shade;
                img.rgb[i + 2] = static_cast<std::uint8_t>(shade + 10);
            }
        }
        std::vector<float> depth(static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
                                 std::numeric_limits<float>::lowest());

        const model::model flat = model::flatten(source);
        const model::bounds box = model::bounds_of(flat);
        if (box.empty())
            return;

        // A camera frame: forward towards the model.
        const math::vec3d eye = math::normalized(v.direction);
        const math::vec3d forward = -eye;
        math::vec3d right = math::cross(forward, math::vec3d{0, 0, 1});
        if (math::magnitude(right) < 1e-6)
            right = {1, 0, 0};
        right = math::normalized(right);
        const math::vec3d up = math::cross(right, forward);
        const math::vec3d light = math::normalized(v.light);
        const math::vec3d centre = box.center().cast<double>();
        const double radius = 0.5 * math::magnitude(box.size().cast<double>());
        const double scale = 0.46 * std::min(width, height) / std::max(radius, 1e-9);

        struct projected
        {
            double x, y, z;
        };
        const auto project = [&](const math::vec3f &p) {
            const math::vec3d d = p.cast<double>() - centre;
            return projected{0.5 * width + math::dot(d, right) * scale, 0.5 * height - math::dot(d, up) * scale,
                             math::dot(d, eye)};
        };

        for (const model::mesh &mesh : flat.meshes)
        {
            const bool per_vertex = mesh.colors.size() == mesh.vertices.size();
            for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3)
            {
                const model::vertex *corner[3] = {&mesh.vertices[mesh.indices[t]], &mesh.vertices[mesh.indices[t + 1]],
                                                  &mesh.vertices[mesh.indices[t + 2]]};
                projected p[3];
                for (int k = 0; k < 3; ++k)
                    p[k] = project(corner[k]->position);
                const double area = (p[1].x - p[0].x) * (p[2].y - p[0].y) - (p[1].y - p[0].y) * (p[2].x - p[0].x);
                if (std::abs(area) < 1e-12)
                    continue;
                const int x0 = std::max(0, static_cast<int>(std::floor(std::min({p[0].x, p[1].x, p[2].x}))));
                const int x1 = std::min(width - 1, static_cast<int>(std::ceil(std::max({p[0].x, p[1].x, p[2].x}))));
                const int y0 = std::max(0, static_cast<int>(std::floor(std::min({p[0].y, p[1].y, p[2].y}))));
                const int y1 = std::min(height - 1, static_cast<int>(std::ceil(std::max({p[0].y, p[1].y, p[2].y}))));
                for (int y = y0; y <= y1; ++y)
                {
                    for (int x = x0; x <= x1; ++x)
                    {
                        const double px = x + 0.5;
                        const double py = y + 0.5;
                        const double w0 = ((p[1].x - px) * (p[2].y - py) - (p[1].y - py) * (p[2].x - px)) / area;
                        const double w1 = ((p[2].x - px) * (p[0].y - py) - (p[2].y - py) * (p[0].x - px)) / area;
                        const double w2 = 1.0 - w0 - w1;
                        if (w0 < 0.0 || w1 < 0.0 || w2 < 0.0)
                            continue;
                        const auto z = static_cast<float>(w0 * p[0].z + w1 * p[1].z + w2 * p[2].z);
                        const std::size_t pixel = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                                  static_cast<std::size_t>(x);
                        if (z <= depth[pixel])
                            continue;
                        depth[pixel] = z;

                        math::vec3d n = corner[0]->normal.cast<double>() * w0 + corner[1]->normal.cast<double>() * w1 +
                                        corner[2]->normal.cast<double>() * w2;
                        const double length = math::magnitude(n);
                        n = length > 0.0 ? n / length : math::vec3d{0, 0, 1};
                        // Back faces (a normal facing away from the eye) in
                        // magenta: they should never be seen on a closed solid.
                        const bool back = math::dot(n, eye) < -0.05;
                        math::vec3d base = (per_vertex ? (mesh.colors[mesh.indices[t]] * static_cast<float>(w0) +
                                                          mesh.colors[mesh.indices[t + 1]] * static_cast<float>(w1) +
                                                          mesh.colors[mesh.indices[t + 2]] * static_cast<float>(w2))
                                                       : mesh.material.color)
                                               .cast<double>();
                        if (back)
                            base = {1.0, 0.0, 1.0};
                        const double lambert = std::max(0.0, math::dot(n, light));
                        const double half = std::max(0.0, math::dot(n, math::normalized(light + eye)));
                        const double specular = std::pow(half, 40.0) * 0.25;
                        math::vec3d linear = base * (0.18 + 0.82 * lambert) + math::vec3d{specular, specular, specular};
                        const std::size_t i = pixel * 3;
                        for (std::size_t c = 0; c < 3; ++c)
                        {
                            const double l = std::clamp(linear[c], 0.0, 1.0);
                            const double s = l <= 0.0031308 ? l * 12.92 : 1.055 * std::pow(l, 1.0 / 2.4) - 0.055;
                            img.rgb[i + c] = static_cast<std::uint8_t>(std::lround(s * 255.0));
                        }
                    }
                }

                if (v.wireframe)
                {
                    for (int k = 0; k < 3; ++k)
                    {
                        const projected &a = p[k];
                        const projected &b = p[(k + 1) % 3];
                        const int steps = static_cast<int>(std::max(std::abs(b.x - a.x), std::abs(b.y - a.y))) + 1;
                        for (int s = 0; s <= steps; ++s)
                        {
                            const double f = static_cast<double>(s) / steps;
                            const int x = static_cast<int>(a.x + (b.x - a.x) * f);
                            const int y = static_cast<int>(a.y + (b.y - a.y) * f);
                            if (x < 0 || y < 0 || x >= width || y >= height)
                                continue;
                            const std::size_t pixel = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                                      static_cast<std::size_t>(x);
                            const auto z = static_cast<float>(a.z + (b.z - a.z) * f);
                            if (z + 1e-3f * static_cast<float>(radius) < depth[pixel])
                                continue;
                            img.rgb[pixel * 3] = 20;
                            img.rgb[pixel * 3 + 1] = 20;
                            img.rgb[pixel * 3 + 2] = 20;
                        }
                    }
                }
            }
        }
    }

} // namespace volt::tests
