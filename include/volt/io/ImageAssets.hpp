#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace volt::io {

struct LoadedImageAsset {
  std::uint32_t width{1};
  std::uint32_t height{1};
  std::vector<std::uint8_t> rgba{0U, 0U, 0U, 0U};
  bool placeholder{true};
  std::string resolvedPath;
};

[[nodiscard]] LoadedImageAsset loadImageAsset(const std::string& textureKey);
[[nodiscard]] bool hasImageAssetChanged(const std::string& textureKey);

}  // namespace volt::io
