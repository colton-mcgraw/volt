#include "volt/io/assets/Manifest.hpp"

#include "volt/io/text/Json.hpp"

#include "volt/core/Logging.hpp"

#include <charconv>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <system_error>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace volt::io {

namespace {

bool areManifestRecordsEqual(
    const std::unordered_map<std::string, ManifestRecord>& lhs,
    const std::unordered_map<std::string, ManifestRecord>& rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }

  for (const auto& [key, value] : lhs) {
    const auto it = rhs.find(key);
    if (it == rhs.end()) {
      return false;
    }
    if (it->second.kind != value.kind) {
      return false;
    }
    if (it->second.value != value.value) {
      return false;
    }
  }

  return true;
}

std::string formatManifestNumber(double value) {
  std::ostringstream stream;
  stream << std::setprecision(std::numeric_limits<double>::digits10 + 1) << value;
  return stream.str();
}

bool equalsAsciiCaseInsensitive(std::string_view lhs, std::string_view rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }

  for (std::size_t index = 0U; index < lhs.size(); ++index) {
    const unsigned char l = static_cast<unsigned char>(lhs[index]);
    const unsigned char r = static_cast<unsigned char>(rhs[index]);
    if (std::tolower(l) != std::tolower(r)) {
      return false;
    }
  }

  return true;
}

std::filesystem::path resolveManifestServicePath() {
  std::error_code ec;
  std::filesystem::path cursor = std::filesystem::current_path(ec);
  if (ec) {
    return std::filesystem::path("assets") / "manifest.json";
  }

  std::filesystem::path fallbackCandidate{};

  for (int depth = 0; depth < 20; ++depth) {
    const std::filesystem::path manifestCandidate = cursor / "assets" / "manifest.json";
    if (std::filesystem::exists(manifestCandidate, ec) && !ec) {
      const std::filesystem::path cmakeListsCandidate = cursor / "CMakeLists.txt";
      if (std::filesystem::exists(cmakeListsCandidate, ec) && !ec) {
        return manifestCandidate.lexically_normal();
      }

      if (fallbackCandidate.empty()) {
        fallbackCandidate = manifestCandidate;
      }
    }

    if (!cursor.has_parent_path()) {
      break;
    }
    const std::filesystem::path parent = cursor.parent_path();
    if (parent == cursor) {
      break;
    }
    cursor = parent;
  }

  if (!fallbackCandidate.empty()) {
    return fallbackCandidate.lexically_normal();
  }

  return (std::filesystem::path("assets") / "manifest.json").lexically_normal();
}

}  // namespace

KeyValueManifest::KeyValueManifest(std::filesystem::path manifestPath)
    : manifestPath_(std::move(manifestPath)) {}

void KeyValueManifest::refresh(bool forceReload) {
  const auto refreshStart = std::chrono::steady_clock::now();
  auto logSlowRefresh = [&]() {
    const double refreshMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - refreshStart).count();
    if (refreshMs >= 10.0) {
      VOLT_LOG_WARN_CAT(
          volt::core::logging::Category::kIO,
          "Slow manifest refresh | path=",
          manifestPath_.string(),
          " | forceReload=",
          forceReload ? "true" : "false",
          " | loaded=",
          loaded_ ? "true" : "false",
          " | disabled=",
          disabled_ ? "true" : "false",
          " | ms=",
          refreshMs);
    }
  };
  const bool wasLoaded = loaded_;
  const bool wasDisabled = disabled_;
  const auto previousTimestamp = manifestTimestamp_;

  std::error_code ec;
  if (!std::filesystem::exists(manifestPath_, ec)) {
    records_.clear();
    loaded_ = false;
    manifestFileSize_ = 0U;
    manifestFileSizeKnown_ = false;
    if (!disabled_) {
      VOLT_LOG_WARN_CAT(
          volt::core::logging::Category::kIO,
          "Manifest not found at ",
          manifestPath_.string(),
          ".");
    }
    disabled_ = true;

    if (wasLoaded || !wasDisabled) {
      notifySubscribers();
    }
    logSlowRefresh();
    return;
  }

  const auto fileSize = std::filesystem::file_size(manifestPath_, ec);
  if (ec) {
    VOLT_LOG_WARN_CAT(
        volt::core::logging::Category::kIO,
        "Unable to read size of manifest '",
        manifestPath_.string(),
        "': ",
        ec.message());
    logSlowRefresh();
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
    logSlowRefresh();
    return;
  }

  // Always parse on refresh to catch edits that keep the same timestamp/size.

  std::ifstream in(manifestPath_);
  if (!in.is_open()) {
    VOLT_LOG_WARN_CAT(
        volt::core::logging::Category::kIO,
        "Unable to open manifest at ",
        manifestPath_.string());
    logSlowRefresh();
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
    logSlowRefresh();
    return;
  }

  const Json::Object* object = parseResult.value.asObject();
  if (object == nullptr) {
    VOLT_LOG_WARN_CAT(
        volt::core::logging::Category::kIO,
        "Manifest root must be a JSON object: ",
        manifestPath_.string());
    logSlowRefresh();
    return;
  }

  std::unordered_map<std::string, ManifestRecord> newRecords;
  for (const auto& [key, value] : *object) {
    if (const std::string* textValue = value.asString(); textValue != nullptr) {
      if (textValue->empty()) {
        continue;
      }

      newRecords[key] = ManifestRecord{*textValue, ManifestRecord::ValueKind::kString};
      continue;
    }

    if (const double* numberValue = value.asNumber(); numberValue != nullptr) {
      newRecords[key] = ManifestRecord{formatManifestNumber(*numberValue), ManifestRecord::ValueKind::kNumber};
      continue;
    }

    if (const bool* boolValue = value.asBoolean(); boolValue != nullptr) {
      newRecords[key] = ManifestRecord{*boolValue ? "true" : "false", ManifestRecord::ValueKind::kBoolean};
      continue;
    }

    if (!value.isNull()) {
      VOLT_LOG_WARN_CAT(
          volt::core::logging::Category::kIO,
          "Skipping manifest key '",
          key,
          "' because the value type is unsupported for key-value lookup");
    }
  }

  const bool recordsChanged = !areManifestRecordsEqual(records_, newRecords);

  records_ = std::move(newRecords);
  manifestTimestamp_ = lastWrite;
  manifestFileSize_ = fileSize;
  manifestFileSizeKnown_ = true;
  loaded_ = true;
  disabled_ = false;

  const bool loadedChanged = loaded_ != wasLoaded;
  const bool disabledChanged = disabled_ != wasDisabled;
  const bool timestampChanged = manifestTimestamp_ != previousTimestamp;

  if (forceReload ||
      recordsChanged ||
      disabledChanged ||
      timestampChanged ||
      loadedChanged ||
      wasDisabled ||
      !wasLoaded ||
      previousTimestamp != lastWrite) {
        VOLT_LOG_INFO_CAT(
            volt::core::logging::Category::kIO,
            "Manifest change detected for ",
            manifestPath_.string(),
            "; notifying subscribers.");
    notifySubscribers();
  }
  logSlowRefresh();
}

KeyValueManifest::SubscriptionId KeyValueManifest::subscribe(ChangeCallback callback) {
  const SubscriptionId id = nextSubscriptionId_++;
  subscribers_.emplace(id, std::move(callback));
  return id;
}

void KeyValueManifest::unsubscribe(SubscriptionId subscriptionId) {
  subscribers_.erase(subscriptionId);
}

void KeyValueManifest::notifySubscribers() const {
  for (const auto& [id, callback] : subscribers_) {
    (void)id;
    if (callback) {
      callback(*this);
    }
  }
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

std::optional<std::string> KeyValueManifest::findString(const std::string& key) const {
  const auto record = find(key);
  if (!record.has_value() || record->kind != ManifestRecord::ValueKind::kString) {
    return std::nullopt;
  }

  return record->value;
}

std::optional<double> KeyValueManifest::findNumber(const std::string& key) const {
  const auto record = find(key);
  if (!record.has_value()) {
    return std::nullopt;
  }

  if (record->kind != ManifestRecord::ValueKind::kNumber &&
      record->kind != ManifestRecord::ValueKind::kString) {
    return std::nullopt;
  }

  double parsed = 0.0;
  const char* begin = record->value.data();
  const char* end = record->value.data() + record->value.size();
  const auto [ptr, ec] = std::from_chars(begin, end, parsed);
  if (ec != std::errc{} || ptr != end) {
    return std::nullopt;
  }

  return parsed;
}

std::optional<bool> KeyValueManifest::findBoolean(const std::string& key) const {
  const auto record = find(key);
  if (!record.has_value()) {
    return std::nullopt;
  }

  if (record->kind == ManifestRecord::ValueKind::kBoolean) {
    return equalsAsciiCaseInsensitive(record->value, "true") || record->value == "1";
  }

  if (record->kind == ManifestRecord::ValueKind::kString) {
    if (equalsAsciiCaseInsensitive(record->value, "true") || record->value == "1") {
      return true;
    }
    if (equalsAsciiCaseInsensitive(record->value, "false") || record->value == "0") {
      return false;
    }
  }

  return std::nullopt;
}

std::optional<std::string> KeyValueManifest::resolvedPathFor(const std::string& key) const {
  const auto stringValue = findString(key);
  if (!stringValue.has_value()) {
    return std::nullopt;
  }

  const std::filesystem::path rawPath(*stringValue);
  if (rawPath.is_absolute()) {
    return rawPath.lexically_normal().string();
  }

  const std::filesystem::path resolved = manifestPath_.parent_path() / rawPath;
  return resolved.lexically_normal().string();
}

KeyValueManifest& manifestService() {
  static KeyValueManifest manifest(resolveManifestServicePath());
  return manifest;
}

}  // namespace volt::io
