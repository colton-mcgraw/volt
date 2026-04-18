#pragma once

#include "volt/render/Renderer.hpp"

#include <memory>

namespace volt::platform {
class Window;
}

namespace volt::render {

enum class RenderBackend {
  kVulkan,
};

[[nodiscard]] std::unique_ptr<Renderer> createRenderer(
    volt::platform::Window& window,
    const char* appName,
    RenderBackend backend = RenderBackend::kVulkan);

}  // namespace volt::render
