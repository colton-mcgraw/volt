#include "volt/core/AppConfig.hpp"
#include "volt/io/ImporterRegistry.hpp"
#include "volt/platform/Window.hpp"
#include "volt/render/VulkanRenderer.hpp"

#include <exception>
#include <iostream>

int main() {
  try {
    const volt::core::AppConfig config{};

    volt::io::ImporterRegistry importers;
    importers.registerDefaultImporters();

    volt::platform::Window window(config.windowWidth, config.windowHeight, config.appName);
    volt::render::VulkanRenderer renderer(window.nativeHandle(), config.appName.c_str());

    while (!window.shouldClose()) {
      window.pollEvents();

      const bool framebufferResized = window.wasResized();
      if (framebufferResized) {
        window.acknowledgeResize();
      }

      if (window.isMinimized()) {
        window.waitEvents();
        continue;
      }

      renderer.tick(framebufferResized);
    }

    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "Fatal error: " << ex.what() << '\n';
    return 1;
  }
}
