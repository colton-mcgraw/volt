// ManifestLoader.hpp
#pragma once

#include "Manifest.hpp"
#include "services/ServiceRegistry.hpp"

#include <filesystem>

class ManifestLoader {
public:
    explicit ManifestLoader(ServiceRegistry& registry)
        : m_registry(registry) {}

    // Initial load — parses manifest and notifies all registered services
    void load(const std::filesystem::path& path) {
        m_path    = path;
        m_manifest = parse(path);
        m_registry.dispatchManifestUpdate(m_manifest);
    }

    // Called by your file watcher (inotify, ReadDirectoryChangesW, etc.)
    void onFileChanged() {
        Manifest updated = parse(m_path);

        // Only dispatch if something actually changed
        if (updated != m_manifest) {
            m_manifest = std::move(updated);
            m_registry.dispatchManifestUpdate(m_manifest);
        }
    }

private:
    Manifest parse(const std::filesystem::path& path);  // your JSON/TOML/etc parser

    ServiceRegistry&          m_registry;
    std::filesystem::path     m_path;
    Manifest                  m_manifest;
};