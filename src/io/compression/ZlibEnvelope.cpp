#include "ZlibEnvelope.hpp"

namespace volt::io::compression::detail {

bool parseZlibEnvelope(const std::vector<std::uint8_t>& compressedData,
                       ZlibEnvelopeView& envelope,
                       std::string& error) {
  envelope = {};

  if (compressedData.size() < 6U) {
    error = "zlib payload too small";
    return false;
  }

  const std::uint8_t cmf = compressedData[0];
  const std::uint8_t flg = compressedData[1];

  const bool checksumOk = ((static_cast<int>(cmf) << 8) + static_cast<int>(flg)) % 31 == 0;
  const bool methodOk = (cmf & 0x0FU) == 8U;
  const bool windowOk = ((cmf >> 4U) <= 7U);
  const bool dictionaryFlagSet = (flg & 0x20U) != 0U;
  if (!checksumOk || !methodOk || !windowOk || dictionaryFlagSet) {
    error = "invalid zlib header";
    return false;
  }

  envelope.expectedAdler =
      (static_cast<std::uint32_t>(compressedData[compressedData.size() - 4U]) << 24U) |
      (static_cast<std::uint32_t>(compressedData[compressedData.size() - 3U]) << 16U) |
      (static_cast<std::uint32_t>(compressedData[compressedData.size() - 2U]) << 8U) |
      static_cast<std::uint32_t>(compressedData[compressedData.size() - 1U]);

  envelope.payloadOffset = 2U;
  envelope.payloadSize = compressedData.size() - 6U;
  return true;
}

void appendU32Be(std::vector<std::uint8_t>& output, std::uint32_t value) {
  output.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
  output.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
  output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
  output.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

}  // namespace volt::io::compression::detail