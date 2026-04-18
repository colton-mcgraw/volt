#pragma once

#include "volt/io/assets/AssetTypes.hpp"

#include <chrono>
#include <cstdint>
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
    std::uint64_t fontAtlasRevision{0U};
    std::uint64_t manifestGeneration{0U};
    std::chrono::steady_clock::time_point nextChangePollTime{};
  };

  [[nodiscard]] std::string resolveImagePathForKey(const std::string& textureKey);

  std::unordered_map<std::string, ImageEntryState> imageEntryState_{};
  std::unordered_map<std::string, LoadedImageAsset> decodedImageCacheByPath_{};
  std::uint64_t manifestGeneration_{0U};
  std::chrono::steady_clock::time_point nextManifestRefreshTime_{};
};

}  // namespace volt::io