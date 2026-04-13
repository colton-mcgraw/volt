#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace volt::io {

enum class CompressionContainer {
  kZlib,
  kRawDeflate,
};

struct CompressionResult {
  bool success{false};
  std::string errorMessage;
};

[[nodiscard]] CompressionResult decompressData(const std::vector<std::uint8_t>& compressedData,
                                               std::vector<std::uint8_t>& decompressedData,
                                               CompressionContainer container = CompressionContainer::kZlib);

[[nodiscard]] CompressionResult compressZlibData(const std::vector<std::uint8_t>& inputData,
                                                 std::vector<std::uint8_t>& compressedData,
                                                 int quality = 8);

}  // namespace volt::io
