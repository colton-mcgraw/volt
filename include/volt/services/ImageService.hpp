// TextureService.hpp

#include "io/manifest/IManifestSubscriber.hpp"
#include "io/manifest/Manifest.hpp"
#include "services/ServiceRegistry.hpp"

#include <filesystem>
#include <memory>

class TextureService {
public:
    // Satisfies ManifestSubscriber
    static constexpr std::string_view manifestKey() { return "textures"; }

    void onManifestUpdated(const ManifestEntry& entry) {
        // Diff against current state — only reload what changed
        auto newBasePath = entry.basePath;

        if (newBasePath != m_basePath) {
            m_basePath = newBasePath;
            reloadAll();
        }

        // Check for format/setting changes
        if (auto it = entry.settings.find("format"); it != entry.settings.end())
            m_format = it->second;
    }

    // Satisfies AssetClient<TextureService, Texture>
    std::shared_ptr<Texture> get(std::string_view id) {
        auto it = m_cache.find(std::string(id));
        if (it != m_cache.end()) return it->second;
        return load(id);
    }

    bool has(std::string_view id) const {
        return m_cache.contains(std::string(id));
    }

private:
    std::shared_ptr<Texture> load(std::string_view id);
    void reloadAll();

    std::filesystem::path                              m_basePath;
    std::string                                        m_format;
    std::unordered_map<std::string, std::shared_ptr<Texture>> m_cache;
};

// Verify contracts at compile time
static_assert(ManifestSubscriber<TextureService>);
static_assert(AssetClient<TextureService, Texture>);