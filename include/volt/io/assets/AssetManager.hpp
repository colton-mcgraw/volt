#pragma once

#include "volt/io/assets/AssetTypes.hpp"
#include "volt/io/assets/FontAssetService.hpp"
#include "volt/io/assets/ImageAssetService.hpp"
#include "volt/io/assets/ModelAssetService.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace volt::io {

class AssetManager {
 public:
  static AssetManager& instance();

  [[nodiscard]] LoadedImageAsset loadImage(const std::string& textureKey);
  [[nodiscard]] bool hasImageChanged(const std::string& textureKey);

  void storeImportedModel(const std::string& assetKey,
                          const std::filesystem::path& sourcePath,
                          ImportResult result);
  [[nodiscard]] std::optional<LoadedModelAsset> findImportedModel(const std::string& assetKey) const;
  [[nodiscard]] bool hasImportedModel(const std::string& assetKey) const;
  void clearImportedModels();

  [[nodiscard]] bool ensureDefaultFontAtlas();
  [[nodiscard]] bool getDefaultFontMetrics(FontMetrics& outMetrics);
  [[nodiscard]] int getDefaultFontGlyphIndexForCodepoint(char32_t codepoint);
  [[nodiscard]] bool getDefaultFontPackedQuad(int glyphIndex, float& x, float& y, FontGlyphQuad& outQuad);
  [[nodiscard]] const std::string& getDefaultFontTextureKey();

 private:
  AssetManager() = default;

  ImageAssetService imageAssets_{};
  ModelAssetService modelAssets_{};
  FontAssetService fontAssets_{};
};

}  // namespace volt::io