#include "volt/render/RendererFactory.hpp"

#include "volt/platform/Window.hpp"
#include "volt/render/details/VulkanRenderer.hpp"

#include <stdexcept>

namespace volt::render {

std::unique_ptr<Renderer> createRenderer(
    volt::platform::Window& window,
    const char* appName,
    RenderBackend backend) {
  switch (backend) {
    case RenderBackend::kVulkan:
      return std::make_unique<details::VulkanRenderer>(window, appName);
    default:
      throw std::invalid_argument("Unsupported renderer backend");
  }
}

}  // namespace volt::render
