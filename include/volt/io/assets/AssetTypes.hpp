#pragma once

#include "volt/io/import/ImportTypes.hpp"

#include <cstdint>
#include <filesystem>
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

struct LoadedModelAsset {
  std::filesystem::path sourcePath;
  ImportResult result;
};

}  // namespace volt::io