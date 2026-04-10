#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace volt::ui {

using FontHandle = std::uint64_t;
using ImageHandle = std::uint64_t;
using FontAtlasHandle = std::uint64_t;

struct GlyphRaster {
  std::uint32_t codepoint{0};
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::int32_t bearingX{0};
  std::int32_t bearingY{0};
  std::uint32_t advanceX{0};
  std::vector<std::uint8_t> alpha;
};

struct FontAtlasBuildRequest {
  FontHandle font{0};
  float fontSizePx{14.0F};
  std::vector<std::uint32_t> codepoints;
};

struct FontAtlasBuildResult {
  FontAtlasHandle atlas{0};
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::vector<GlyphRaster> glyphs;
};

class IFontLoader {
 public:
  virtual ~IFontLoader() = default;

  [[nodiscard]] virtual std::optional<FontHandle> loadFont(
      const std::string& fontFamily,
      const std::string& fontPath) = 0;
};

class IImageLoader {
 public:
  virtual ~IImageLoader() = default;

  [[nodiscard]] virtual std::optional<ImageHandle> loadImage(
      const std::string& imageKey,
      const std::string& imagePath) = 0;
};

class IFontAtlasBuilder {
 public:
  virtual ~IFontAtlasBuilder() = default;

  [[nodiscard]] virtual std::optional<FontAtlasBuildResult> buildAtlas(
    const FontAtlasBuildRequest& request) = 0;
};

class UIResourceRegistry {
 public:
  void setFontLoader(IFontLoader* loader) {
    fontLoader_ = loader;
  }

  void setImageLoader(IImageLoader* loader) {
    imageLoader_ = loader;
  }

  void setFontAtlasBuilder(IFontAtlasBuilder* builder) {
    fontAtlasBuilder_ = builder;
  }

  [[nodiscard]] std::optional<FontHandle> resolveFont(const std::string& fontFamily) const {
    const auto it = fonts_.find(fontFamily);
    if (it == fonts_.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  [[nodiscard]] std::optional<ImageHandle> resolveImage(const std::string& imageKey) const {
    const auto it = images_.find(imageKey);
    if (it == images_.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  void registerFont(const std::string& fontFamily, FontHandle handle) {
    fonts_[fontFamily] = handle;
  }

  void registerImage(const std::string& imageKey, ImageHandle handle) {
    images_[imageKey] = handle;
  }

  void registerFontAtlas(const std::string& key, FontAtlasHandle handle) {
    fontAtlases_[key] = handle;
  }

  [[nodiscard]] std::optional<FontAtlasHandle> resolveFontAtlas(const std::string& key) const {
    const auto it = fontAtlases_.find(key);
    if (it == fontAtlases_.end()) {
      return std::nullopt;
    }
    return it->second;
  }

 private:
  IFontLoader* fontLoader_{nullptr};
  IImageLoader* imageLoader_{nullptr};
  IFontAtlasBuilder* fontAtlasBuilder_{nullptr};
  std::unordered_map<std::string, FontHandle> fonts_;
  std::unordered_map<std::string, ImageHandle> images_;
  std::unordered_map<std::string, FontAtlasHandle> fontAtlases_;
};

}  // namespace volt::ui
