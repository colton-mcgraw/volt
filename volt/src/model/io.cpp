#include <model/io.hpp>

#include <model/obj.hpp>
#include <model/stl.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>

namespace volt::model
{

    namespace
    {
        std::string lower(std::string_view text)
        {
            std::string result(text);
            for (char &c : result)
            {
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            }
            if (!result.empty() && result.front() == '.')
                result.erase(0, 1);
            return result;
        }

        format from_extension(std::string_view extension)
        {
            const std::string e = lower(extension);
            if (e == "step" || e == "stp" || e == "p21")
                return format::step;
            if (e == "obj")
                return format::obj;
            if (e == "stl")
                return format::stl;
            return format::unknown;
        }

        std::expected<std::string, std::string> read_file(const std::filesystem::path &path)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return std::unexpected("cannot open " + path.string());
            in.seekg(0, std::ios::end);
            const std::streamoff size = in.tellg();
            if (size < 0)
                return std::unexpected("cannot read " + path.string());
            std::string bytes(static_cast<std::size_t>(size), '\0');
            in.seekg(0, std::ios::beg);
            in.read(bytes.data(), size);
            if (!in)
                return std::unexpected("cannot read " + path.string());
            return bytes;
        }

        std::expected<void, std::string> write_file(const std::filesystem::path &path, std::string_view bytes)
        {
            std::ofstream out(path, std::ios::binary);
            if (!out)
                return std::unexpected("cannot create " + path.string());
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (!out)
                return std::unexpected("cannot write " + path.string());
            return {};
        }

        std::expected<loaded, std::string> load_with(std::string_view bytes, format kind, const load_options &options,
                                                      const obj::material_source &materials)
        {
            loaded result;
            const auto start = std::chrono::steady_clock::now();
            std::expected<model, std::string> read = std::unexpected(std::string("unknown format"));
            switch (kind)
            {
            case format::step:
                read = step::read(bytes, options, result.report);
                break;
            case format::obj:
                read = obj::read(bytes, materials, result.report);
                break;
            case format::stl:
                read = stl::read(bytes, result.report);
                break;
            case format::unknown:
                return std::unexpected("not a model file this can read (STEP, OBJ or STL)");
            }
            if (kind != format::step)
                result.report.parse_seconds =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            if (!read)
                return std::unexpected(read.error());
            result.model = std::move(*read);
            return result;
        }
    } // namespace

    format detect(std::string_view bytes, std::string_view extension)
    {
        std::size_t start = 0;
        if (bytes.substr(0, 3) == "\xEF\xBB\xBF")
            start = 3;
        while (start < bytes.size() && (bytes[start] == ' ' || bytes[start] == '\t' || bytes[start] == '\r' ||
                                        bytes[start] == '\n'))
            ++start;
        const std::string_view text = bytes.substr(start);

        if (text.starts_with("ISO-10303-21"))
            return format::step;
        if (stl::is_binary(bytes))
            return format::stl;
        if (text.starts_with("solid") && text.substr(0, 4096).find("facet") != std::string_view::npos)
            return format::stl;

        const format by_extension = from_extension(extension);
        if (by_extension != format::unknown)
            return by_extension;

        // OBJ has no signature; its lines start with a handful of keys.
        const std::string_view head = text.substr(0, 4096);
        for (std::string_view key : {"v ", "vn ", "f ", "o ", "g ", "mtllib ", "usemtl "})
        {
            if (head.starts_with(key) || head.find(std::string("\n") + std::string(key)) != std::string_view::npos)
                return format::obj;
        }
        return format::unknown;
    }

    format format_of(const std::filesystem::path &path)
    {
        return from_extension(path.extension().string());
    }

    std::expected<loaded, std::string> load(const std::filesystem::path &path, const load_options &options)
    {
        const auto bytes = read_file(path);
        if (!bytes)
            return std::unexpected(bytes.error());
        const format kind = detect(*bytes, path.extension().string());

        const std::filesystem::path directory = path.parent_path();
        const obj::material_source materials = [&](std::string_view name) -> std::optional<std::string> {
            // Only next to the OBJ or below it: a material library name is
            // not a way out of the directory.
            const std::filesystem::path candidate = (directory / std::filesystem::path(name)).lexically_normal();
            const std::string relative = candidate.lexically_relative(directory).string();
            if (relative.empty() || relative.starts_with(".."))
                return std::nullopt;
            auto text = read_file(candidate);
            if (!text)
                return std::nullopt;
            return std::move(*text);
        };

        auto result = load_with(*bytes, kind, options, materials);
        if (!result)
            return std::unexpected(path.filename().string() + ": " + result.error());
        if (result->model.name.empty())
            result->model.name = path.stem().string();
        return result;
    }

    std::expected<loaded, std::string> load(std::string_view bytes, format kind, const load_options &options)
    {
        return load_with(bytes, kind, options, {});
    }

    std::expected<void, std::string> save(const std::filesystem::path &path, const model &model,
                                          const save_options &options)
    {
        switch (format_of(path))
        {
        case format::step:
        {
            step::write_options step = options.step;
            if (step.file_name.empty())
                step.file_name = path.filename().string();
            return write_file(path, step::write(model, step));
        }
        case format::obj:
        {
            std::filesystem::path mtl = path;
            mtl.replace_extension(".mtl");
            const obj::written written = obj::write(model, mtl.filename().string());
            if (auto saved = write_file(path, written.obj); !saved)
                return saved;
            return write_file(mtl, written.mtl);
        }
        case format::stl:
            return write_file(path, options.ascii_stl ? stl::write_ascii(model) : stl::write_binary(model));
        case format::unknown:
            break;
        }
        return std::unexpected("cannot tell the format to write from the file name " + path.filename().string() +
                               " (.step, .stp, .obj or .stl)");
    }

} // namespace volt::model
