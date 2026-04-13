#pragma once

#include "volt/io/assets/AssetTypes.hpp"

#include <filesystem>
#include <string>
#include <unordered_map>

namespace volt::io {

class ImageAssetService {
 public:
  [[nodiscard]] LoadedImageAsset loadImage(const std::string& textureKey);
  [[nodiscard]] bool hasImageChanged(const std::string& textureKey);

 private:
  struct ImageEntryState {
    std::string resolvedPath;
    std::filesystem::file_time_type lastWrite{};
    bool hasWriteTime{false};
  };

  [[nodiscard]] std::string resolveImagePathForKey(const std::string& textureKey);

  std::unordered_map<std::string, ImageEntryState> imageEntryState_{};
};

}  // namespace volt::io