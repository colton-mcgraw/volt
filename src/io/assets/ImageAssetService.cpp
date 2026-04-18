#include "volt/io/assets/ImageAssetService.hpp"

#include "volt/io/assets/Font.hpp"
#include "volt/io/assets/Manifest.hpp"
#include "volt/io/image/ImageDecoder.hpp"

#include "volt/core/Logging.hpp"

#include <array>
#include <cctype>
#include <chrono>
#include <string_view>

namespace volt::io {
namespace {

constexpr auto kImageHotReloadPollInterval = std::chrono::milliseconds(250);

std::array<std::uint8_t, 4> fallbackColorFromKey(const std::string& key) {
  std::uint32_t hash = 2166136261u;
  for (unsigned char ch : key) {
    hash ^= static_cast<std::uint32_t>(ch);
    hash *= 16777619u;
  }
  return {static_cast<std::uint8_t>((hash >> 16) & 0xFFu),
          static_cast<std::uint8_t>((hash >> 8) & 0xFFu),
          static_cast<std::uint8_t>(hash & 0xFFu),
          255u};
}

LoadedImageAsset makeBlankPlaceholderImage(const std::string& key) {
  LoadedImageAsset image{};
  image.width = 1;
  image.height = 1;
  const auto color = fallbackColorFromKey(key);
  image.rgba = {color[0], color[1], color[2], color[3]};
  image.placeholder = true;
  return image;
}

bool endsWithCaseInsensitive(const std::string& text, const std::string& suffix) {
  if (suffix.size() > text.size()) {
    return false;
  }

  const std::size_t offset = text.size() - suffix.size();
  for (std::size_t i = 0; i < suffix.size(); ++i) {
    const auto lhs = static_cast<unsigned char>(text[offset + i]);
    const auto rhs = static_cast<unsigned char>(suffix[i]);
    if (std::tolower(lhs) != std::tolower(rhs)) {
      return false;
    }
  }

  return true;
}

std::string normalizeImageKey(const std::string& textureKey) {
  std::string normalizedKey = textureKey;
  constexpr std::string_view prefix = "image:";
  if (normalizedKey.rfind(prefix.data(), 0) == 0) {
    normalizedKey.erase(0, prefix.size());
  }
  return normalizedKey;
}

}  // namespace

std::string ImageAssetService::resolveImagePathForKey(const std::string& textureKey) {
  if (textureKey.empty() || textureKey == "__white") {
    return {};
  }

  const std::string normalizedKey = normalizeImageKey(textureKey);

  auto& manifest = manifestService();
  manifest.refresh(false);
  if (!manifest.isDisabled()) {
    const auto resolved = manifest.resolvedPathFor(normalizedKey);
    if (resolved.has_value()) {
      return resolved.value();
    }
  }

  static const std::array<std::string, 4> candidates = {
      std::string("assets/images/") + normalizedKey + ".png",
      std::string("assets/images/") + normalizedKey + ".jpg",
      std::string("assets/images/") + normalizedKey + ".jpeg",
      std::string("assets/images/") + normalizedKey + ".bmp",
  };

  for (const auto& candidate : candidates) {
    std::error_code ec;
    if (std::filesystem::exists(candidate, ec) && !ec) {
      return candidate;
    }
  }

  return {};
}

LoadedImageAsset ImageAssetService::loadImage(const std::string& textureKey) {
  std::string path = resolveImagePathForKey(textureKey);
  if (path.empty()) {
    return makeBlankPlaceholderImage(textureKey);
  }

  if (const auto cachedImage = decodedImageCacheByPath_.find(path);
      cachedImage != decodedImageCacheByPath_.end()) {
    return cachedImage->second;
  }

  LoadedImageAsset image{};
  image.resolvedPath = path;

  if (endsWithCaseInsensitive(path, ".svg")) {
    auto placeholder = makeBlankPlaceholderImage(textureKey);
    placeholder.resolvedPath = path;
    decodedImageCacheByPath_[path] = placeholder;
    return placeholder;
  }

  RawImage decoded{};
  if (!decodeImageFile(path, decoded)) {
    VOLT_LOG_WARN_CAT(
        volt::core::logging::Category::kIO,
        "Failed to decode image '",
        path,
        "'; using placeholder for key '",
        textureKey,
        "'");
    auto placeholder = makeBlankPlaceholderImage(textureKey);
    placeholder.resolvedPath = path;
    decodedImageCacheByPath_[path] = placeholder;
    return placeholder;
  }

  image.width = decoded.width;
  image.height = decoded.height;
  image.rgba = std::move(decoded.rgba);
  image.placeholder = false;
  decodedImageCacheByPath_[path] = image;
  return image;
}

bool ImageAssetService::hasImageChanged(const std::string& textureKey) {
  const auto imageCheckStart = std::chrono::steady_clock::now();
  const auto now = imageCheckStart;
  auto logSlowCheck = [&](std::string_view reason) {
    const double imageCheckMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - imageCheckStart).count();
    if (imageCheckMs >= 10.0) {
      VOLT_LOG_WARN_CAT(
          volt::core::logging::Category::kIO,
          "Slow image hot-reload check | key=",
          textureKey,
          " | reason=",
          reason,
          " | ms=",
          imageCheckMs);
    }
  };
  if (textureKey.empty() || textureKey == "__white") {
    return false;
  }

  const std::string normalizedKey = normalizeImageKey(textureKey);
  const bool isDefaultFontTextureKey = normalizedKey == "ui-font-default";
  const std::uint64_t fontAtlasRevision = isDefaultFontTextureKey ? defaultFontAtlasRevision() : 0U;
  auto it = imageEntryState_.find(normalizedKey);

  auto& manifest = manifestService();
  if (now >= nextManifestRefreshTime_) {
    const auto oldManifestTimestamp = manifest.manifestTimestamp();
    manifest.refresh(false);
    if (manifest.manifestTimestamp() != oldManifestTimestamp) {
      ++manifestGeneration_;
    }
    nextManifestRefreshTime_ = now + kImageHotReloadPollInterval;
  }

  if (it != imageEntryState_.end()) {
    if (isDefaultFontTextureKey && it->second.fontAtlasRevision != fontAtlasRevision) {
      decodedImageCacheByPath_.erase(it->second.resolvedPath);
      it->second.fontAtlasRevision = fontAtlasRevision;
      it->second.nextChangePollTime = now;
      logSlowCheck("fontAtlasRevisionChanged");
      return true;
    }

    if (it->second.manifestGeneration == manifestGeneration_ && now < it->second.nextChangePollTime) {
      return false;
    }
  }

  const auto newResolvedPath = manifest.resolvedPathFor(normalizedKey);

  if (!newResolvedPath.has_value()) {
    if (it != imageEntryState_.end()) {
      decodedImageCacheByPath_.erase(it->second.resolvedPath);
      imageEntryState_.erase(it);
      logSlowCheck("resolvedPathRemoved");
      return true;
    }
    logSlowCheck("resolvedPathMissing");
    return false;
  }

  if (it == imageEntryState_.end() || it->second.resolvedPath != newResolvedPath.value()) {
    ImageEntryState entry{};
    entry.resolvedPath = newResolvedPath.value();
    entry.fontAtlasRevision = fontAtlasRevision;
    entry.manifestGeneration = manifestGeneration_;
    entry.nextChangePollTime = now + kImageHotReloadPollInterval;

    if (it != imageEntryState_.end()) {
      decodedImageCacheByPath_.erase(it->second.resolvedPath);
    }

    std::error_code ec;
    const auto writeTime = std::filesystem::last_write_time(entry.resolvedPath, ec);
    if (!ec) {
      entry.lastWrite = writeTime;
      entry.hasWriteTime = true;
    }

    const bool existed = (it != imageEntryState_.end());
    imageEntryState_[normalizedKey] = entry;
    logSlowCheck(existed ? "resolvedPathChanged" : "entryCreated");
    return existed;
  }

  it->second.manifestGeneration = manifestGeneration_;
  it->second.nextChangePollTime = now + kImageHotReloadPollInterval;

  std::error_code ec;
  const auto currentTime = std::filesystem::last_write_time(newResolvedPath.value(), ec);
  if (ec) {
    decodedImageCacheByPath_.erase(newResolvedPath.value());
    logSlowCheck("lastWriteTimeError");
    return false;
  }

  if (!it->second.hasWriteTime || currentTime != it->second.lastWrite) {
    decodedImageCacheByPath_.erase(newResolvedPath.value());
    it->second.lastWrite = currentTime;
    it->second.hasWriteTime = true;
    it->second.nextChangePollTime = now + kImageHotReloadPollInterval;
    logSlowCheck("fileWriteTimeChanged");
    return true;
  }

  logSlowCheck("unchanged");
  return false;
}

}  // namespace volt::io