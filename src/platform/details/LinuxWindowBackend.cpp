#include "volt/platform/details/LinuxWindowBackend.hpp"

#include <stdexcept>
#include <utility>

namespace volt::platform::details {

LinuxWindowBackend::LinuxWindowBackend(
    std::uint32_t width,
    std::uint32_t height,
    const std::string& title,
    const WindowCreateOptions& options) {
  (void)width;
  (void)height;
  (void)title;
  (void)options;
  throw std::runtime_error("Linux window backend scaffold is not implemented yet");
}

bool LinuxWindowBackend::shouldClose() const {
  return true;
}

void LinuxWindowBackend::requestClose() {}

void LinuxWindowBackend::setEventDispatcher(volt::event::EventDispatcher* dispatcher) {
  (void)dispatcher;
}

void LinuxWindowBackend::setResizeRepaintCallback(std::function<void()> callback) {
  (void)callback;
}

void LinuxWindowBackend::pollEvents() {}

void LinuxWindowBackend::waitEvents() {}

void LinuxWindowBackend::waitEventsTimeout(double timeoutSeconds) {
  (void)timeoutSeconds;
}

bool LinuxWindowBackend::wasResized() const {
  return false;
}

void LinuxWindowBackend::acknowledgeResize() {}

bool LinuxWindowBackend::isMinimized() const {
  return false;
}

std::pair<std::uint32_t, std::uint32_t> LinuxWindowBackend::framebufferExtent() const {
  return {0U, 0U};
}

std::pair<std::uint32_t, std::uint32_t> LinuxWindowBackend::logicalExtent() const {
  return framebufferExtent();
}

DisplayMetrics LinuxWindowBackend::displayMetrics() const {
  return {};
}

const InputState& LinuxWindowBackend::inputSnapshot() const {
  return inputState_;
}

WindowBackendType LinuxWindowBackend::backendType() const {
  return WindowBackendType::kLinux;
}

void* LinuxWindowBackend::nativeWindowHandle() const {
  return nullptr;
}

void* LinuxWindowBackend::nativeDisplayHandle() const {
  return nullptr;
}

void LinuxWindowBackend::beginInteractiveMove() {}

void LinuxWindowBackend::beginInteractiveResize(WindowResizeEdge edge) {
  (void)edge;
}

bool LinuxWindowBackend::isMaximized() const {
  return false;
}

void LinuxWindowBackend::toggleMaximized() {}

void LinuxWindowBackend::minimize() {}

}  // namespace volt::platform::details
