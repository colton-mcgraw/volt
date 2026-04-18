#pragma once

#include "volt/platform/WindowBackend.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <cstdint>
#include <string>

namespace volt::platform::details {

class Win32WindowBackend final : public WindowBackend {
 public:
  Win32WindowBackend(
      std::uint32_t width,
      std::uint32_t height,
      const std::string& title,
      const WindowCreateOptions& options);
  ~Win32WindowBackend() override;

  Win32WindowBackend(const Win32WindowBackend&) = delete;
  Win32WindowBackend& operator=(const Win32WindowBackend&) = delete;

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
  void resetTransientInputState();
  void processPendingMessages();
  void publishResizeEvent(std::uint32_t width, std::uint32_t height);
  void publishMinimizedEvent(bool minimized);
  int modifierMask() const;
  void updateDisplayMetrics();
  [[nodiscard]] double physicalToLogicalX(double value) const;
  [[nodiscard]] double physicalToLogicalY(double value) const;

#if defined(_WIN32)
  LRESULT handleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam);
  static LRESULT CALLBACK windowProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

  HWND hwnd_{nullptr};
  HINSTANCE hinstance_{nullptr};
  HICON iconBig_{nullptr};
  HICON iconSmall_{nullptr};
  wchar_t pendingHighSurrogate_{0};
#endif

  bool shouldClose_{false};
  bool framebufferResized_{false};
  bool minimized_{false};
  WindowCreateOptions options_{};
  DisplayMetrics displayMetrics_{};
  InputState inputState_{};
  volt::event::EventDispatcher* eventDispatcher_{nullptr};
  std::function<void()> resizeRepaintCallback_{};
};

}  // namespace volt::platform::details
