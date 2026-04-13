#include "volt/io/assets/Manifest.hpp"

#include "volt/io/text/Json.hpp"

#include "volt/core/Logging.hpp"

#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>

namespace volt::io {

KeyValueManifest::KeyValueManifest(std::filesystem::path manifestPath)
    : manifestPath_(std::move(manifestPath)) {}

void KeyValueManifest::refresh(bool forceReload) {
  std::error_code ec;
  if (!std::filesystem::exists(manifestPath_, ec)) {
    records_.clear();
    loaded_ = false;
    if (!disabled_) {
      VOLT_LOG_WARN_CAT(
          volt::core::logging::Category::kIO,
          "Manifest not found at ",
          manifestPath_.string(),
          ".");
    }
    disabled_ = true;
    return;
  }

  const auto lastWrite = std::filesystem::last_write_time(manifestPath_, ec);
  if (ec) {
    VOLT_LOG_WARN_CAT(
        volt::core::logging::Category::kIO,
        "Unable to stat manifest '",
        manifestPath_.string(),
        "': ",
        ec.message());
    return;
  }

  if (!forceReload && loaded_ && lastWrite == manifestTimestamp_) {
    return;
  }

  std::ifstream in(manifestPath_);
  if (!in.is_open()) {
    VOLT_LOG_WARN_CAT(
        volt::core::logging::Category::kIO,
        "Unable to open manifest at ",
        manifestPath_.string());
    return;
  }

  const std::string manifestText{
      std::istreambuf_iterator<char>{in},
      std::istreambuf_iterator<char>{}};
  const JsonParseResult parseResult = parseJson(manifestText);
  if (!parseResult.success) {
    VOLT_LOG_WARN_CAT(
        volt::core::logging::Category::kIO,
        "Failed to parse JSON manifest '",
        manifestPath_.string(),
        "': ",
        parseResult.errorMessage);
    return;
  }

  const Json::Object* object = parseResult.value.asObject();
  if (object == nullptr) {
    VOLT_LOG_WARN_CAT(
        volt::core::logging::Category::kIO,
        "Manifest root must be a JSON object: ",
        manifestPath_.string());
    return;
  }

  std::unordered_map<std::string, ManifestRecord> newRecords;
  for (const auto& [key, value] : *object) {
    const std::string* pathValue = value.asString();
    if (pathValue == nullptr || pathValue->empty()) {
      VOLT_LOG_WARN_CAT(
          volt::core::logging::Category::kIO,
          "Skipping manifest key '",
          key,
          "' because the value is not a non-empty string");
      continue;
    }

    newRecords[key] = ManifestRecord{*pathValue};
  }

  records_ = std::move(newRecords);
  manifestTimestamp_ = lastWrite;
  loaded_ = true;
  disabled_ = false;

  VOLT_LOG_INFO_CAT(
      volt::core::logging::Category::kIO,
      "Loaded ",
      records_.size(),
      " records from manifest ",
      manifestPath_.string());
}

bool KeyValueManifest::isLoaded() const {
  return loaded_;
}

bool KeyValueManifest::isDisabled() const {
  return disabled_;
}

const std::filesystem::path& KeyValueManifest::manifestPath() const {
  return manifestPath_;
}

std::filesystem::file_time_type KeyValueManifest::manifestTimestamp() const {
  return manifestTimestamp_;
}

std::optional<ManifestRecord> KeyValueManifest::find(const std::string& key) const {
  const auto it = records_.find(key);
  if (it == records_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::optional<std::string> KeyValueManifest::resolvedPathFor(const std::string& key) const {
  const auto record = find(key);
  if (!record.has_value()) {
    return std::nullopt;
  }

  const std::filesystem::path rawPath(record->value);
  if (rawPath.is_absolute()) {
    return rawPath.lexically_normal().string();
  }

  const std::filesystem::path resolved = manifestPath_.parent_path() / rawPath;
  return resolved.lexically_normal().string();
}

}  // namespace volt::io
