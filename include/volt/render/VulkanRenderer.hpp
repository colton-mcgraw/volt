#pragma once

#include <optional>
#include <vector>
#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>

namespace volt::render {

class VulkanRenderer {
 public:
  VulkanRenderer(GLFWwindow* window, const char* appName);
  ~VulkanRenderer();

  VulkanRenderer(const VulkanRenderer&) = delete;
  VulkanRenderer& operator=(const VulkanRenderer&) = delete;

  void tick(bool framebufferResized);

 private:
  struct QueueFamilyIndices {
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> presentFamily;

    [[nodiscard]] bool isComplete() const;
  };

  struct SwapchainSupportDetails {
    VkSurfaceCapabilitiesKHR capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
  };

  void createInstance(const char* appName);
  void createSurface();
  void pickPhysicalDevice();
  void createLogicalDevice();
  void createSwapchain();
  void createSwapchainImageViews();

  void recreateSwapchain();
  void cleanupSwapchain();

  [[nodiscard]] QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device) const;
  [[nodiscard]] SwapchainSupportDetails querySwapchainSupport(VkPhysicalDevice device) const;

  [[nodiscard]] bool isDeviceSuitable(VkPhysicalDevice device) const;

  [[nodiscard]] VkSurfaceFormatKHR chooseSwapSurfaceFormat(
      const std::vector<VkSurfaceFormatKHR>& availableFormats) const;
  [[nodiscard]] VkPresentModeKHR chooseSwapPresentMode(
      const std::vector<VkPresentModeKHR>& availablePresentModes) const;
  [[nodiscard]] VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities) const;

  GLFWwindow* window_{nullptr};
  VkInstance instance_{VK_NULL_HANDLE};
  VkSurfaceKHR surface_{VK_NULL_HANDLE};
  VkPhysicalDevice physicalDevice_{VK_NULL_HANDLE};
  VkDevice device_{VK_NULL_HANDLE};
  VkQueue graphicsQueue_{VK_NULL_HANDLE};
  VkQueue presentQueue_{VK_NULL_HANDLE};

  VkSwapchainKHR swapchain_{VK_NULL_HANDLE};
  std::vector<VkImage> swapchainImages_;
  std::vector<VkImageView> swapchainImageViews_;
  VkFormat swapchainImageFormat_{VK_FORMAT_UNDEFINED};
  VkExtent2D swapchainExtent_{};
};

}  // namespace volt::render
