#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

namespace volt::io {

struct ManifestRecord {
  std::string value;
};

class KeyValueManifest {
 public:
  explicit KeyValueManifest(std::filesystem::path manifestPath);

  void refresh(bool forceReload = false);

  [[nodiscard]] bool isLoaded() const;
  [[nodiscard]] bool isDisabled() const;
  [[nodiscard]] const std::filesystem::path& manifestPath() const;
  [[nodiscard]] std::filesystem::file_time_type manifestTimestamp() const;

  [[nodiscard]] std::optional<ManifestRecord> find(const std::string& key) const;
  [[nodiscard]] std::optional<std::string> resolvedPathFor(const std::string& key) const;

 private:
  std::filesystem::path manifestPath_;
  bool loaded_{false};
  bool disabled_{false};
  std::filesystem::file_time_type manifestTimestamp_{};
  std::unordered_map<std::string, ManifestRecord> records_;
};

}  // namespace volt::io
