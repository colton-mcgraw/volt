// IManifestSubscriber.hpp
#pragma once

#include <string_view>

// Contract a service must fulfill to subscribe to manifest updates.
// 'entry' is the service's named section from the manifest.
template<typename T>
concept ManifestSubscriber = requires(T t, const ManifestEntry& entry) {
    // Called when the manifest is first loaded or hot-reloaded.
    // Service decides internally what changed and what to reload.
    { t.onManifestUpdated(entry) } -> std::same_as<void>;

    // Unique key identifying this service's manifest section.
    { T::manifestKey() } -> std::convertible_to<std::string_view>;
};

// Contract a client must fulfill to consume an asset service.
template<typename TService, typename THandle>
concept AssetClient = requires(TService svc, std::string_view id) {
    // Request a loaded asset by ID. Returns a shared handle.
    { svc.get(id) } -> std::same_as<std::shared_ptr<THandle>>;

    // Check availability without triggering a load.
    { svc.has(id) } -> std::same_as<bool>;
};