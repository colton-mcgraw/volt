#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace volt::io::compression::detail {

struct ZlibEnvelopeView {
  std::size_t payloadOffset{0U};
  std::size_t payloadSize{0U};
  std::uint32_t expectedAdler{0U};
};

bool parseZlibEnvelope(const std::vector<std::uint8_t>& compressedData,
                       ZlibEnvelopeView& envelope,
                       std::string& error);

void appendU32Be(std::vector<std::uint8_t>& output, std::uint32_t value);

}  // namespace volt::io::compression::detail