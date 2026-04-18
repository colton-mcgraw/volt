// ServiceRegistry.hpp
#pragma once

#include <functional>
#include <unordered_map>
#include <memory>
#include "io/manifest/IManifestSubscriber.hpp"

class ServiceRegistry {
public:
    // Register a service that satisfies ManifestSubscriber.
    // The registry owns the subscription slot; the service owns itself.
    template<ManifestSubscriber T>
    void registerService(std::shared_ptr<T> service) {
        std::string key{ T::manifestKey() };

        // Store a type-erased update callback
        m_subscribers[key] = [svc = std::weak_ptr<T>(service)](const ManifestEntry& entry) {
            if (auto s = svc.lock())
                s->onManifestUpdated(entry);
        };

        // Store the service itself for client lookup
        m_services[key] = service;
    }

    // Client-facing: retrieve a service by its manifest key.
    // Returns nullptr if not registered.
    template<typename T>
    std::shared_ptr<T> get(std::string_view key) const {
        auto it = m_services.find(std::string(key));
        if (it == m_services.end()) return nullptr;
        return std::static_pointer_cast<T>(it->second);
    }

    // Called by the manifest loader on load or hot-reload.
    void dispatchManifestUpdate(const Manifest& manifest) {
        for (auto& [key, callback] : m_subscribers) {
            if (auto* entry = manifest.find(key))
                callback(*entry);
        }
    }

private:
    using UpdateCallback = std::function<void(const ManifestEntry&)>;

    std::unordered_map<std::string, UpdateCallback>          m_subscribers;
    std::unordered_map<std::string, std::shared_ptr<void>>   m_services;
};