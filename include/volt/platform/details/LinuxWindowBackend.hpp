#pragma once

#include "volt/platform/WindowBackend.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace volt::platform::details {

class LinuxWindowBackend final : public WindowBackend {
 public:
  LinuxWindowBackend(
      std::uint32_t width,
      std::uint32_t height,
      const std::string& title,
      const WindowCreateOptions& options);
  ~LinuxWindowBackend() override = default;

  [[nodiscard]] bool shouldClose() const override;
  void requestClose() override;
  void setEventDispatcher(volt::event::EventDispatcher* dispatcher) override;
  void setResizeRepaintCallback(std::function<void()> callback) override;

  void pollEvents() override;
  void waitEvents() override;
  void waitEventsTimeout(double timeoutSeconds) override;

  [[nodiscard]] bool wasResized() const override;
  void acknowledgeResize() override;

  [[nodiscard]] bool isMinimized() const override;
  [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> framebufferExtent() const override;
  [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> logicalExtent() const override;
  [[nodiscard]] DisplayMetrics displayMetrics() const override;
  [[nodiscard]] const InputState& inputSnapshot() const override;

  [[nodiscard]] WindowBackendType backendType() const override;
  [[nodiscard]] void* nativeWindowHandle() const override;
  [[nodiscard]] void* nativeDisplayHandle() const override;

  void beginInteractiveMove() override;
  void beginInteractiveResize(WindowResizeEdge edge) override;
  [[nodiscard]] bool isMaximized() const override;
  void toggleMaximized() override;
  void minimize() override;

 private:
  InputState inputState_{};
};

}  // namespace volt::platform::details
