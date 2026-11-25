#pragma once

#include <image/compress.hpp>

// The inflater underneath compress::inflate, for the wrappers that need to
// know where the DEFLATE stream ended. Internal to the image library.

namespace volt::compress::detail
{

    // Appends the decompressed stream to `output` (which counts against
    // options.max_output) and returns how many input bytes it took, the
    // final partial byte included.
    std::expected<std::size_t, std::string> inflate_append(std::span<const std::uint8_t> input,
                                                           std::vector<std::uint8_t> &output,
                                                           const inflate_options &options);

} // namespace volt::compress::detail
