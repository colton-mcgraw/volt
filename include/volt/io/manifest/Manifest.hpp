// Manifest.hpp
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>


struct ManifestEntry {
    std::string_view            serviceKey;
    std::filesystem::path       basePath;
    std::unordered_map<std::string, std::string> settings;   // arbitrary key/value from manifest
};

struct Manifest {
    std::unordered_map<std::string, ManifestEntry> entries;

    const ManifestEntry* find(std::string_view key) const {
        auto it = entries.find(std::string(key));
        return it != entries.end() ? &it->second : nullptr;
    }
};