#include "volt/io/assets/ModelAssetService.hpp"

#include <utility>

namespace volt::io {

void ModelAssetService::storeImportedModel(const std::string& assetKey,
                                           const std::filesystem::path& sourcePath,
                                           ImportResult result) {
  if (assetKey.empty()) {
    return;
  }

  modelAssets_[assetKey] = LoadedModelAsset{.sourcePath = sourcePath, .result = std::move(result)};
}

std::optional<LoadedModelAsset> ModelAssetService::findImportedModel(const std::string& assetKey) const {
  const auto it = modelAssets_.find(assetKey);
  if (it == modelAssets_.end()) {
    return std::nullopt;
  }

  return it->second;
}

bool ModelAssetService::hasImportedModel(const std::string& assetKey) const {
  return modelAssets_.find(assetKey) != modelAssets_.end();
}

void ModelAssetService::clearImportedModels() {
  modelAssets_.clear();
}

}  // namespace volt::io