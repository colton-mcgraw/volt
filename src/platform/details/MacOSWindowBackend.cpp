#include "volt/platform/details/MacOSWindowBackend.hpp"

#include <stdexcept>
#include <utility>

namespace volt::platform::details {

MacOSWindowBackend::MacOSWindowBackend(
    std::uint32_t width,
    std::uint32_t height,
    const std::string& title,
    const WindowCreateOptions& options) {
  (void)width;
  (void)height;
  (void)title;
  (void)options;
  throw std::runtime_error("macOS window backend scaffold is not implemented yet");
}

bool MacOSWindowBackend::shouldClose() const {
  return true;
}

void MacOSWindowBackend::requestClose() {}

void MacOSWindowBackend::setEventDispatcher(volt::event::EventDispatcher* dispatcher) {
  (void)dispatcher;
}

void MacOSWindowBackend::setResizeRepaintCallback(std::function<void()> callback) {
  (void)callback;
}

void MacOSWindowBackend::pollEvents() {}

void MacOSWindowBackend::waitEvents() {}

void MacOSWindowBackend::waitEventsTimeout(double timeoutSeconds) {
  (void)timeoutSeconds;
}

bool MacOSWindowBackend::wasResized() const {
  return false;
}

void MacOSWindowBackend::acknowledgeResize() {}

bool MacOSWindowBackend::isMinimized() const {
  return false;
}

std::pair<std::uint32_t, std::uint32_t> MacOSWindowBackend::framebufferExtent() const {
  return {0U, 0U};
}

std::pair<std::uint32_t, std::uint32_t> MacOSWindowBackend::logicalExtent() const {
  return framebufferExtent();
}

DisplayMetrics MacOSWindowBackend::displayMetrics() const {
  return {};
}

const InputState& MacOSWindowBackend::inputSnapshot() const {
  return inputState_;
}

WindowBackendType MacOSWindowBackend::backendType() const {
  return WindowBackendType::kMacOS;
}

void* MacOSWindowBackend::nativeWindowHandle() const {
  return nullptr;
}

void* MacOSWindowBackend::nativeDisplayHandle() const {
  return nullptr;
}

void MacOSWindowBackend::beginInteractiveMove() {}

void MacOSWindowBackend::beginInteractiveResize(WindowResizeEdge edge) {
  (void)edge;
}

bool MacOSWindowBackend::isMaximized() const {
  return false;
}

void MacOSWindowBackend::toggleMaximized() {}

void MacOSWindowBackend::minimize() {}

}  // namespace volt::platform::details
