#include "volt/io/assets/ImageAssetService.hpp"

#include "volt/io/assets/Manifest.hpp"
#include "volt/io/image/ImageDecoder.hpp"

#include "volt/core/Logging.hpp"

#include <array>
#include <cctype>
#include <string_view>

namespace volt::io {
namespace {

KeyValueManifest& imageManifest() {
  static KeyValueManifest manifest(std::filesystem::path("assets") / "manifest.json");
  return manifest;
}

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

  auto& manifest = imageManifest();
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

  LoadedImageAsset image{};
  image.resolvedPath = path;

  if (endsWithCaseInsensitive(path, ".svg")) {
    auto placeholder = makeBlankPlaceholderImage(textureKey);
    placeholder.resolvedPath = path;
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
    return placeholder;
  }

  image.width = decoded.width;
  image.height = decoded.height;
  image.rgba = std::move(decoded.rgba);
  image.placeholder = false;
  return image;
}

bool ImageAssetService::hasImageChanged(const std::string& textureKey) {
  if (textureKey.empty() || textureKey == "__white") {
    return false;
  }

  const std::string normalizedKey = normalizeImageKey(textureKey);

  auto& manifest = imageManifest();
  const auto oldManifestTimestamp = manifest.manifestTimestamp();
  manifest.refresh(false);
  if (manifest.manifestTimestamp() != oldManifestTimestamp) {
    return true;
  }

  const auto newResolvedPath = manifest.resolvedPathFor(normalizedKey);
  auto it = imageEntryState_.find(normalizedKey);

  if (!newResolvedPath.has_value()) {
    if (it != imageEntryState_.end()) {
      imageEntryState_.erase(it);
      return true;
    }
    return false;
  }

  if (it == imageEntryState_.end() || it->second.resolvedPath != newResolvedPath.value()) {
    ImageEntryState entry{};
    entry.resolvedPath = newResolvedPath.value();

    std::error_code ec;
    const auto writeTime = std::filesystem::last_write_time(entry.resolvedPath, ec);
    if (!ec) {
      entry.lastWrite = writeTime;
      entry.hasWriteTime = true;
    }

    const bool existed = (it != imageEntryState_.end());
    imageEntryState_[normalizedKey] = entry;
    return existed;
  }

  std::error_code ec;
  const auto currentTime = std::filesystem::last_write_time(newResolvedPath.value(), ec);
  if (ec) {
    return false;
  }

  if (!it->second.hasWriteTime || currentTime != it->second.lastWrite) {
    it->second.lastWrite = currentTime;
    it->second.hasWriteTime = true;
    return true;
  }

  return false;
}

}  // namespace volt::io