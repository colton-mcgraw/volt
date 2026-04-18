#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

namespace volt::io {

struct ManifestRecord {
  enum class ValueKind {
    kString,
    kNumber,
    kBoolean,
  };

  std::string value;
  ValueKind kind{ValueKind::kString};
};

class KeyValueManifest {
 public:
  using SubscriptionId = std::uint64_t;
  using ChangeCallback = std::function<void(const KeyValueManifest&)>;

  explicit KeyValueManifest(std::filesystem::path manifestPath);

  void refresh(bool forceReload = false);
  [[nodiscard]] SubscriptionId subscribe(ChangeCallback callback);
  void unsubscribe(SubscriptionId subscriptionId);

  [[nodiscard]] bool isLoaded() const;
  [[nodiscard]] bool isDisabled() const;
  [[nodiscard]] const std::filesystem::path& manifestPath() const;
  [[nodiscard]] std::filesystem::file_time_type manifestTimestamp() const;

  [[nodiscard]] std::optional<ManifestRecord> find(const std::string& key) const;
  [[nodiscard]] std::optional<std::string> findString(const std::string& key) const;
  [[nodiscard]] std::optional<double> findNumber(const std::string& key) const;
  [[nodiscard]] std::optional<bool> findBoolean(const std::string& key) const;
  [[nodiscard]] std::optional<std::string> resolvedPathFor(const std::string& key) const;

 private:
  void notifySubscribers() const;

  std::filesystem::path manifestPath_;
  bool loaded_{false};
  bool disabled_{false};
  std::filesystem::file_time_type manifestTimestamp_{};
  std::uintmax_t manifestFileSize_{0U};
  bool manifestFileSizeKnown_{false};
  std::unordered_map<std::string, ManifestRecord> records_;
  SubscriptionId nextSubscriptionId_{1U};
  std::unordered_map<SubscriptionId, ChangeCallback> subscribers_;
};

[[nodiscard]] KeyValueManifest& manifestService();

}  // namespace volt::io
