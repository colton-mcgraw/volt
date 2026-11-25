#include <image/io.hpp>

#include <image/bmp.hpp>

#include <algorithm>
#include <format>
#include <fstream>
#include <system_error>

namespace volt::image
{

    const char *name(file_format format) noexcept
    {
        switch (format)
        {
        case file_format::png:
            return "PNG";
        case file_format::jpeg:
            return "JPEG";
        case file_format::bmp:
            return "BMP";
        case file_format::svg:
            return "SVG";
        case file_format::unknown:
            break;
        }
        return "unknown";
    }

    file_format detect(std::span<const std::uint8_t> data) noexcept
    {
        if (png::is_png(data))
            return file_format::png;
        if (jpeg::is_jpeg(data))
            return file_format::jpeg;
        if (bmp::is_bmp(data))
            return file_format::bmp;
        if (svg::is_svg(data))
            return file_format::svg;
        return file_format::unknown;
    }

    file_format format_from_extension(const std::filesystem::path &path) noexcept
    {
        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
        if (extension == ".png")
            return file_format::png;
        if (extension == ".jpg" || extension == ".jpeg" || extension == ".jpe" || extension == ".jfif")
            return file_format::jpeg;
        if (extension == ".bmp" || extension == ".dib")
            return file_format::bmp;
        if (extension == ".svg" || extension == ".svgz")
            return file_format::svg;
        return file_format::unknown;
    }

    std::expected<image, std::string> load(std::span<const std::uint8_t> data, const load_options &options)
    {
        switch (detect(data))
        {
        case file_format::png:
            return png::decode(data);
        case file_format::jpeg:
            return jpeg::decode(data, options.jpeg);
        case file_format::bmp:
            return bmp::decode(data);
        case file_format::svg:
        {
            auto document = svg::parse(data);
            if (!document)
                return std::unexpected(std::move(document.error()));
            return svg::rasterize(*document, options.svg);
        }
        case file_format::unknown:
            break;
        }
        if (data.empty())
            return std::unexpected(std::string("the file is empty"));
        return std::unexpected(std::string("the data is not an image in a format Volt reads (PNG, JPEG, BMP or SVG)"));
    }

    std::expected<image, std::string> load(const std::filesystem::path &path, const load_options &options)
    {
        auto bytes = read_file(path);
        if (!bytes)
            return std::unexpected(std::move(bytes.error()));
        auto result = load(*bytes, options);
        if (!result)
            return std::unexpected(std::format("{}: {}", path.filename().string(), result.error()));
        return result;
    }

    std::expected<std::vector<std::uint8_t>, std::string> encode(const image &source, file_format format,
                                                                 const save_options &options)
    {
        switch (format)
        {
        case file_format::png:
            return png::encode(source, options.png);
        case file_format::jpeg:
            return jpeg::encode(source, options.jpeg);
        case file_format::bmp:
        case file_format::svg:
            return std::unexpected(std::format("Volt reads {} files but does not write them", name(format)));
        case file_format::unknown:
            break;
        }
        return std::unexpected(std::string("no image format was given to encode to"));
    }

    std::expected<void, std::string> save(const std::filesystem::path &path, const image &source,
                                          const save_options &options)
    {
        const file_format format = format_from_extension(path);
        if (format == file_format::unknown)
            return std::unexpected(std::format("{}: the extension does not name an image format Volt writes "
                                               "(.png, .jpg)",
                                               path.filename().string()));
        auto bytes = encode(source, format, options);
        if (!bytes)
            return std::unexpected(std::format("{}: {}", path.filename().string(), bytes.error()));
        return write_file(path, *bytes);
    }

    // ---- files ------------------------------------------------------------------

    std::expected<std::vector<std::uint8_t>, std::string> read_file(const std::filesystem::path &path,
                                                                    std::uintmax_t limit)
    {
        const std::string shown = path.filename().string();
        std::error_code error;
        const std::uintmax_t size = std::filesystem::file_size(path, error);
        if (error)
            return std::unexpected(std::format("{}: cannot read the file ({})", shown, error.message()));
        if (size > limit)
            return std::unexpected(std::format("{}: the file is {} bytes, more than the {} allowed", shown, size, limit));

        std::ifstream file(path, std::ios::binary);
        if (!file)
            return std::unexpected(std::format("{}: cannot open the file", shown));
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (file.gcount() != static_cast<std::streamsize>(bytes.size()))
            return std::unexpected(std::format("{}: reading the file failed", shown));
        return bytes;
    }

    std::expected<void, std::string> write_file(const std::filesystem::path &path, std::span<const std::uint8_t> data)
    {
        const std::string shown = path.filename().string();
        std::filesystem::path temporary = path;
        temporary += ".partial";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (!file)
                return std::unexpected(std::format("{}: cannot create the file", shown));
            file.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
            file.flush();
            if (!file)
            {
                file.close();
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                return std::unexpected(std::format("{}: writing the file failed", shown));
            }
        }
        std::error_code error;
        std::filesystem::rename(temporary, path, error);
        if (error)
        {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return std::unexpected(std::format("{}: cannot replace the file ({})", shown, error.message()));
        }
        return {};
    }

} // namespace volt::image
