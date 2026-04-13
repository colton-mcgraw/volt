#include "volt/io/assets/AssetManager.hpp"

#include <utility>

namespace volt::io {

AssetManager& AssetManager::instance() {
  static AssetManager manager;
  return manager;
}

LoadedImageAsset AssetManager::loadImage(const std::string& textureKey) {
  return imageAssets_.loadImage(textureKey);
}

bool AssetManager::hasImageChanged(const std::string& textureKey) {
  return imageAssets_.hasImageChanged(textureKey);
}

void AssetManager::storeImportedModel(const std::string& assetKey,
                                      const std::filesystem::path& sourcePath,
                                      ImportResult result) {
  modelAssets_.storeImportedModel(assetKey, sourcePath, std::move(result));
}

std::optional<LoadedModelAsset> AssetManager::findImportedModel(const std::string& assetKey) const {
  return modelAssets_.findImportedModel(assetKey);
}

bool AssetManager::hasImportedModel(const std::string& assetKey) const {
  return modelAssets_.hasImportedModel(assetKey);
}

void AssetManager::clearImportedModels() {
  modelAssets_.clearImportedModels();
}

bool AssetManager::ensureDefaultFontAtlas() {
  return fontAssets_.ensureDefaultFontAtlas();
}

bool AssetManager::getDefaultFontMetrics(FontMetrics& outMetrics) {
  return fontAssets_.getDefaultFontMetrics(outMetrics);
}

int AssetManager::getDefaultFontGlyphIndexForCodepoint(char32_t codepoint) {
  return fontAssets_.getDefaultFontGlyphIndexForCodepoint(codepoint);
}

bool AssetManager::getDefaultFontPackedQuad(int glyphIndex, float& x, float& y, FontGlyphQuad& outQuad) {
  return fontAssets_.getDefaultFontPackedQuad(glyphIndex, x, y, outQuad);
}

const std::string& AssetManager::getDefaultFontTextureKey() {
  return fontAssets_.getDefaultFontTextureKey();
}

}  // namespace volt::io