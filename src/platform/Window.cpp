#include "volt/platform/Window.hpp"

#include <stdexcept>
#include <utility>

namespace volt::platform {

Window::Window(std::uint32_t width, std::uint32_t height, const std::string& title) {
  if (glfwRefCount_ == 0) {
    if (glfwInit() != GLFW_TRUE) {
      throw std::runtime_error("Failed to initialize GLFW");
    }
  }
  ++glfwRefCount_;

  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

  window_ = glfwCreateWindow(
      static_cast<int>(width),
      static_cast<int>(height),
      title.c_str(),
      nullptr,
      nullptr);

  if (window_ == nullptr) {
    --glfwRefCount_;
    if (glfwRefCount_ == 0) {
      glfwTerminate();
    }
    throw std::runtime_error("Failed to create GLFW window");
  }

  glfwSetWindowUserPointer(window_, this);
  glfwSetFramebufferSizeCallback(window_, framebufferSizeCallback);
  glfwSetWindowIconifyCallback(window_, iconifyCallback);
}

Window::~Window() {
  if (window_ != nullptr) {
    glfwDestroyWindow(window_);
    window_ = nullptr;
  }

  --glfwRefCount_;
  if (glfwRefCount_ == 0) {
    glfwTerminate();
  }
}

bool Window::shouldClose() const {
  return glfwWindowShouldClose(window_) == GLFW_TRUE;
}

void Window::requestClose() const {
  glfwSetWindowShouldClose(window_, GLFW_TRUE);
}

void Window::pollEvents() const {
  glfwPollEvents();
}

void Window::waitEvents() const {
  glfwWaitEvents();
}

bool Window::wasResized() const {
  return framebufferResized_;
}

void Window::acknowledgeResize() {
  framebufferResized_ = false;
}

bool Window::isMinimized() const {
  return minimized_;
}

std::pair<std::uint32_t, std::uint32_t> Window::framebufferExtent() const {
  int width = 0;
  int height = 0;
  glfwGetFramebufferSize(window_, &width, &height);
  return {
      static_cast<std::uint32_t>(width > 0 ? width : 0),
      static_cast<std::uint32_t>(height > 0 ? height : 0),
  };
}

GLFWwindow* Window::nativeHandle() const {
  return window_;
}

void Window::framebufferSizeCallback(GLFWwindow* window, int width, int height) {
  auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
  if (self == nullptr) {
    return;
  }

  self->framebufferResized_ = true;
  self->minimized_ = (width == 0 || height == 0);
}

void Window::iconifyCallback(GLFWwindow* window, int iconified) {
  auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
  if (self == nullptr) {
    return;
  }

  self->minimized_ = (iconified == GLFW_TRUE);
}

}  // namespace volt::platform
