#pragma once

#include "volt/platform/InputState.hpp"

#include <GLFW/glfw3.h>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace volt::event {
class EventDispatcher;
}

namespace volt::platform {

class Window {
 public:
  Window(std::uint32_t width, std::uint32_t height, const std::string& title);
  ~Window();

  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;

  [[nodiscard]] bool shouldClose() const;
  void requestClose() const;
  void setEventDispatcher(volt::event::EventDispatcher* dispatcher);
  void setResizeRepaintCallback(std::function<void()> callback);

  void pollEvents();
  void waitEvents();
  void waitEventsTimeout(double timeoutSeconds);

  [[nodiscard]] bool wasResized() const;
  void acknowledgeResize();

  [[nodiscard]] bool isMinimized() const;
  [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> framebufferExtent() const;
  [[nodiscard]] const InputState& inputSnapshot() const;

  [[nodiscard]] GLFWwindow* nativeHandle() const;

 private:
  void resetTransientInputState();

  static void framebufferSizeCallback(GLFWwindow* window, int width, int height);
  static void refreshCallback(GLFWwindow* window);
  static void iconifyCallback(GLFWwindow* window, int iconified);
  static void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
  static void cursorPosCallback(GLFWwindow* window, double xPos, double yPos);
  static void mouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
  static void scrollCallback(GLFWwindow* window, double xOffset, double yOffset);

  static inline int glfwRefCount_{0};
  GLFWwindow* window_{nullptr};
  bool framebufferResized_{false};
  bool minimized_{false};
  InputState inputState_{};
  volt::event::EventDispatcher* eventDispatcher_{nullptr};
  std::function<void()> resizeRepaintCallback_{};
};

}  // namespace volt::platform
