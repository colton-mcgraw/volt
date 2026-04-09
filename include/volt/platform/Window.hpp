#pragma once

#include <GLFW/glfw3.h>
#include <cstdint>
#include <utility>
#include <string>

namespace volt::platform {

class Window {
 public:
  Window(std::uint32_t width, std::uint32_t height, const std::string& title);
  ~Window();

  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;

  [[nodiscard]] bool shouldClose() const;
  void requestClose() const;

  void pollEvents() const;
  void waitEvents() const;

  [[nodiscard]] bool wasResized() const;
  void acknowledgeResize();

  [[nodiscard]] bool isMinimized() const;
  [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> framebufferExtent() const;

  [[nodiscard]] GLFWwindow* nativeHandle() const;

 private:
  static void framebufferSizeCallback(GLFWwindow* window, int width, int height);
  static void iconifyCallback(GLFWwindow* window, int iconified);

  static inline int glfwRefCount_{0};
  GLFWwindow* window_{nullptr};
  bool framebufferResized_{false};
  bool minimized_{false};
};

}  // namespace volt::platform
