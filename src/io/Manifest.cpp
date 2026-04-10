#include "volt/io/Manifest.hpp"

#include "volt/core/Logging.hpp"

#include <cctype>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>

namespace volt::io {
namespace {

std::string trim(std::string_view input) {
  std::size_t start = 0;
  std::size_t end = input.size();
  while (start < end && std::isspace(static_cast<unsigned char>(input[start])) != 0) {
    ++start;
  }
  while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
    --end;
  }
  return std::string(input.substr(start, end - start));
}

}  // namespace

KeyValueManifest::KeyValueManifest(std::filesystem::path manifestPath)
    : manifestPath_(std::move(manifestPath)) {}

void KeyValueManifest::refresh(bool forceReload) {
  if (disabled_) {
    return;
  }

  std::error_code ec;
  if (!std::filesystem::exists(manifestPath_, ec)) {
    records_.clear();
    loaded_ = true;
    disabled_ = true;
    VOLT_LOG_WARN_CAT(
        volt::core::logging::Category::kIO,
        "Manifest not found at ",
        manifestPath_.string(),
        ".");
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

  std::unordered_map<std::string, ManifestRecord> newRecords;
  std::string line;
  std::size_t lineNo = 0;
  while (std::getline(in, line)) {
    ++lineNo;
    const std::string trimmed = trim(line);
    if (trimmed.empty() || trimmed[0] == '#') {
      continue;
    }

    const std::size_t eq = trimmed.find('=');
    if (eq == std::string::npos) {
      VOLT_LOG_WARN_CAT(
          volt::core::logging::Category::kIO,
          "Skipping malformed manifest line ",
          lineNo,
          ": ",
          trimmed);
      continue;
    }

    const std::string key = trim(std::string_view(trimmed).substr(0, eq));
    const std::string value = trim(std::string_view(trimmed).substr(eq + 1));
    if (key.empty() || value.empty()) {
      VOLT_LOG_WARN_CAT(
          volt::core::logging::Category::kIO,
          "Skipping malformed manifest line ",
          lineNo,
          ": ",
          trimmed);
      continue;
    }

    newRecords[key] = ManifestRecord{value};
  }

  records_ = std::move(newRecords);
  manifestTimestamp_ = lastWrite;
  loaded_ = true;

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
