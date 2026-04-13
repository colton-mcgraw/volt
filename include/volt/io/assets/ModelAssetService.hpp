#pragma once

#include "volt/io/assets/AssetTypes.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

namespace volt::io {

class ModelAssetService {
 public:
  void storeImportedModel(const std::string& assetKey,
                          const std::filesystem::path& sourcePath,
                          ImportResult result);
  [[nodiscard]] std::optional<LoadedModelAsset> findImportedModel(const std::string& assetKey) const;
  [[nodiscard]] bool hasImportedModel(const std::string& assetKey) const;
  void clearImportedModels();

 private:
  std::unordered_map<std::string, LoadedModelAsset> modelAssets_{};
};

}  // namespace volt::io