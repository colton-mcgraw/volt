#include "volt/render/VulkanRenderer.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <vector>

namespace volt::render {

namespace {
constexpr std::uint32_t kRendererVersionMajor = 0;
constexpr std::uint32_t kRendererVersionMinor = 1;
constexpr std::uint32_t kRendererVersionPatch = 0;
}  // namespace

bool VulkanRenderer::QueueFamilyIndices::isComplete() const {
  return graphicsFamily.has_value() && presentFamily.has_value();
}

VulkanRenderer::VulkanRenderer(GLFWwindow* window, const char* appName) : window_(window) {
  if (window_ == nullptr) {
    throw std::invalid_argument("VulkanRenderer requires a valid window handle");
  }

  createInstance(appName);
  createSurface();
  pickPhysicalDevice();
  createLogicalDevice();
  createSwapchain();
  createSwapchainImageViews();
}

VulkanRenderer::~VulkanRenderer() {
  if (device_ != VK_NULL_HANDLE) {
    vkDeviceWaitIdle(device_);
  }

  cleanupSwapchain();

  if (device_ != VK_NULL_HANDLE) {
    vkDestroyDevice(device_, nullptr);
    device_ = VK_NULL_HANDLE;
  }

  if (surface_ != VK_NULL_HANDLE) {
    vkDestroySurfaceKHR(instance_, surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
  }

  if (instance_ != VK_NULL_HANDLE) {
    vkDestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;
  }
}

void VulkanRenderer::tick(bool framebufferResized) {
  if (swapchain_ == VK_NULL_HANDLE) {
    return;
  }

  if (framebufferResized) {
    recreateSwapchain();
    return;
  }

  int width = 0;
  int height = 0;
  glfwGetFramebufferSize(window_, &width, &height);
  if (width == 0 || height == 0) {
    return;
  }

  if (static_cast<std::uint32_t>(width) != swapchainExtent_.width ||
      static_cast<std::uint32_t>(height) != swapchainExtent_.height) {
    recreateSwapchain();
    return;
  }

  // Rendering pipeline and UI integration entry point.
}

void VulkanRenderer::createInstance(const char* appName) {
  uint32_t extensionCount = 0;
  const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&extensionCount);
  if (glfwExtensions == nullptr || extensionCount == 0) {
    throw std::runtime_error("GLFW did not provide Vulkan instance extensions");
  }

  std::vector<const char*> requiredExtensions(glfwExtensions, glfwExtensions + extensionCount);

  VkApplicationInfo appInfo{};
  appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  appInfo.pApplicationName = appName;
    appInfo.applicationVersion = VK_MAKE_VERSION(
      kRendererVersionMajor,
      kRendererVersionMinor,
      kRendererVersionPatch);
  appInfo.pEngineName = "VoltEngine";
    appInfo.engineVersion = VK_MAKE_VERSION(
      kRendererVersionMajor,
      kRendererVersionMinor,
      kRendererVersionPatch);
  appInfo.apiVersion = VK_API_VERSION_1_3;

  VkInstanceCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  createInfo.pApplicationInfo = &appInfo;
  createInfo.enabledExtensionCount = static_cast<uint32_t>(requiredExtensions.size());
  createInfo.ppEnabledExtensionNames = requiredExtensions.data();

  const VkResult result = vkCreateInstance(&createInfo, nullptr, &instance_);
  if (result != VK_SUCCESS) {
    throw std::runtime_error("Failed to create Vulkan instance");
  }
}

void VulkanRenderer::createSurface() {
  const VkResult result = glfwCreateWindowSurface(instance_, window_, nullptr, &surface_);
  if (result != VK_SUCCESS) {
    throw std::runtime_error("Failed to create Vulkan surface");
  }
}

void VulkanRenderer::pickPhysicalDevice() {
  uint32_t deviceCount = 0;
  vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr);
  if (deviceCount == 0) {
    throw std::runtime_error("No Vulkan physical device found");
  }

  std::vector<VkPhysicalDevice> devices(deviceCount);
  vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data());

  for (VkPhysicalDevice device : devices) {
    if (isDeviceSuitable(device)) {
      physicalDevice_ = device;
      break;
    }
  }

  if (physicalDevice_ == VK_NULL_HANDLE) {
    throw std::runtime_error("No suitable Vulkan physical device found");
  }
}

void VulkanRenderer::createLogicalDevice() {
  const QueueFamilyIndices indices = findQueueFamilies(physicalDevice_);

  std::set<uint32_t> uniqueQueueFamilies = {
      indices.graphicsFamily.value(),
      indices.presentFamily.value(),
  };

  constexpr float queuePriority = 1.0F;
  std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
  queueCreateInfos.reserve(uniqueQueueFamilies.size());

  for (uint32_t queueFamily : uniqueQueueFamilies) {
    VkDeviceQueueCreateInfo queueCreateInfo{};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = queueFamily;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;
    queueCreateInfos.push_back(queueCreateInfo);
  }

  const std::vector<const char*> requiredDeviceExtensions = {
      VK_KHR_SWAPCHAIN_EXTENSION_NAME,
  };

  VkPhysicalDeviceFeatures deviceFeatures{};

  VkDeviceCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
  createInfo.pQueueCreateInfos = queueCreateInfos.data();
  createInfo.pEnabledFeatures = &deviceFeatures;
  createInfo.enabledExtensionCount = static_cast<uint32_t>(requiredDeviceExtensions.size());
  createInfo.ppEnabledExtensionNames = requiredDeviceExtensions.data();

  const VkResult result = vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_);
  if (result != VK_SUCCESS) {
    throw std::runtime_error("Failed to create Vulkan logical device");
  }

  vkGetDeviceQueue(device_, indices.graphicsFamily.value(), 0, &graphicsQueue_);
  vkGetDeviceQueue(device_, indices.presentFamily.value(), 0, &presentQueue_);
}

void VulkanRenderer::createSwapchain() {
  const SwapchainSupportDetails support = querySwapchainSupport(physicalDevice_);

  const VkSurfaceFormatKHR surfaceFormat = chooseSwapSurfaceFormat(support.formats);
  const VkPresentModeKHR presentMode = chooseSwapPresentMode(support.presentModes);
  const VkExtent2D extent = chooseSwapExtent(support.capabilities);

  uint32_t imageCount = support.capabilities.minImageCount + 1;
  if (support.capabilities.maxImageCount > 0 && imageCount > support.capabilities.maxImageCount) {
    imageCount = support.capabilities.maxImageCount;
  }

  VkSwapchainCreateInfoKHR createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  createInfo.surface = surface_;
  createInfo.minImageCount = imageCount;
  createInfo.imageFormat = surfaceFormat.format;
  createInfo.imageColorSpace = surfaceFormat.colorSpace;
  createInfo.imageExtent = extent;
  createInfo.imageArrayLayers = 1;
  createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

  const QueueFamilyIndices indices = findQueueFamilies(physicalDevice_);
  const uint32_t queueFamilyIndices[] = {
      indices.graphicsFamily.value(),
      indices.presentFamily.value(),
  };

  if (indices.graphicsFamily != indices.presentFamily) {
    createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
    createInfo.queueFamilyIndexCount = 2;
    createInfo.pQueueFamilyIndices = queueFamilyIndices;
  } else {
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  }

  createInfo.preTransform = support.capabilities.currentTransform;
  createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  createInfo.presentMode = presentMode;
  createInfo.clipped = VK_TRUE;
  createInfo.oldSwapchain = VK_NULL_HANDLE;

  const VkResult result = vkCreateSwapchainKHR(device_, &createInfo, nullptr, &swapchain_);
  if (result != VK_SUCCESS) {
    throw std::runtime_error("Failed to create Vulkan swapchain");
  }

  vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr);
  swapchainImages_.resize(imageCount);
  vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, swapchainImages_.data());

  swapchainImageFormat_ = surfaceFormat.format;
  swapchainExtent_ = extent;
}

void VulkanRenderer::createSwapchainImageViews() {
  swapchainImageViews_.resize(swapchainImages_.size());

  for (size_t i = 0; i < swapchainImages_.size(); ++i) {
    VkImageViewCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    createInfo.image = swapchainImages_[i];
    createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    createInfo.format = swapchainImageFormat_;
    createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    createInfo.subresourceRange.baseMipLevel = 0;
    createInfo.subresourceRange.levelCount = 1;
    createInfo.subresourceRange.baseArrayLayer = 0;
    createInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device_, &createInfo, nullptr, &swapchainImageViews_[i]) != VK_SUCCESS) {
      throw std::runtime_error("Failed to create Vulkan swapchain image view");
    }
  }
}

void VulkanRenderer::recreateSwapchain() {
  int width = 0;
  int height = 0;
  glfwGetFramebufferSize(window_, &width, &height);

  while (width == 0 || height == 0) {
    glfwWaitEvents();
    glfwGetFramebufferSize(window_, &width, &height);
  }

  vkDeviceWaitIdle(device_);
  cleanupSwapchain();
  createSwapchain();
  createSwapchainImageViews();
}

void VulkanRenderer::cleanupSwapchain() {
  for (VkImageView imageView : swapchainImageViews_) {
    vkDestroyImageView(device_, imageView, nullptr);
  }
  swapchainImageViews_.clear();

  if (swapchain_ != VK_NULL_HANDLE) {
    vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
  }
  swapchainImages_.clear();
}

VulkanRenderer::QueueFamilyIndices VulkanRenderer::findQueueFamilies(VkPhysicalDevice device) const {
  QueueFamilyIndices indices;

  uint32_t queueFamilyCount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
  std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
  vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

  for (uint32_t i = 0; i < queueFamilyCount; ++i) {
    if ((queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
      indices.graphicsFamily = i;
    }

    VkBool32 presentSupport = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &presentSupport);
    if (presentSupport == VK_TRUE) {
      indices.presentFamily = i;
    }

    if (indices.isComplete()) {
      break;
    }
  }

  return indices;
}

VulkanRenderer::SwapchainSupportDetails VulkanRenderer::querySwapchainSupport(VkPhysicalDevice device) const {
  SwapchainSupportDetails details;

  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface_, &details.capabilities);

  uint32_t formatCount = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &formatCount, nullptr);
  if (formatCount > 0) {
    details.formats.resize(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &formatCount, details.formats.data());
  }

  uint32_t presentModeCount = 0;
  vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &presentModeCount, nullptr);
  if (presentModeCount > 0) {
    details.presentModes.resize(presentModeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(
        device,
        surface_,
        &presentModeCount,
        details.presentModes.data());
  }

  return details;
}

bool VulkanRenderer::isDeviceSuitable(VkPhysicalDevice device) const {
  const QueueFamilyIndices indices = findQueueFamilies(device);
  if (!indices.isComplete()) {
    return false;
  }

  const SwapchainSupportDetails support = querySwapchainSupport(device);
  if (support.formats.empty() || support.presentModes.empty()) {
    return false;
  }

  return true;
}

VkSurfaceFormatKHR VulkanRenderer::chooseSwapSurfaceFormat(
    const std::vector<VkSurfaceFormatKHR>& availableFormats) const {
  for (const VkSurfaceFormatKHR& availableFormat : availableFormats) {
    if (availableFormat.format == VK_FORMAT_B8G8R8A8_SRGB &&
        availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      return availableFormat;
    }
  }

  return availableFormats.front();
}

VkPresentModeKHR VulkanRenderer::chooseSwapPresentMode(
    const std::vector<VkPresentModeKHR>& availablePresentModes) const {
  for (VkPresentModeKHR availablePresentMode : availablePresentModes) {
    if (availablePresentMode == VK_PRESENT_MODE_MAILBOX_KHR) {
      return availablePresentMode;
    }
  }

  return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D VulkanRenderer::chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities) const {
  if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
    return capabilities.currentExtent;
  }

  int width = 0;
  int height = 0;
  glfwGetFramebufferSize(window_, &width, &height);

  VkExtent2D actualExtent = {
      static_cast<uint32_t>(width > 0 ? width : 0),
      static_cast<uint32_t>(height > 0 ? height : 0),
  };

  actualExtent.width = std::clamp(
      actualExtent.width,
      capabilities.minImageExtent.width,
      capabilities.maxImageExtent.width);

  actualExtent.height = std::clamp(
      actualExtent.height,
      capabilities.minImageExtent.height,
      capabilities.maxImageExtent.height);

  return actualExtent;
}

}  // namespace volt::render
