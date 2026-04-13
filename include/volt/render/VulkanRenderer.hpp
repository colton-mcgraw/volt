#pragma once

#include "volt/math/Matrix.hpp"
#include "volt/math/Vector.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <string>
#include <unordered_set>
#include <vector>
#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>

namespace volt::event {
class EventDispatcher;
}

namespace volt::ui {
struct UiMeshData;
}

namespace volt::render {

class VulkanRenderer {
 public:
  VulkanRenderer(GLFWwindow* window, const char* appName);
  ~VulkanRenderer();

  VulkanRenderer(const VulkanRenderer&) = delete;
  VulkanRenderer& operator=(const VulkanRenderer&) = delete;

  struct SceneSubmission {
    std::uint32_t meshCount{0};
    std::uint32_t instanceCount{0};
  };

  using UiPassCallback = std::function<void(VkCommandBuffer, std::uint32_t, std::uint32_t)>;
  using UiMeshProvider = std::function<const volt::ui::UiMeshData*()>;

  void submitScene(const SceneSubmission& submission);
  void setUiPassCallback(UiPassCallback callback);
  void setUiMeshProvider(UiMeshProvider provider);
  void setEventDispatcher(volt::event::EventDispatcher* dispatcher);
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
  void setupDebugMessenger();
  void cleanupDebugMessenger();
  void createSurface();
  void pickPhysicalDevice();
  void createLogicalDevice();
  void createSwapchain();
  void createSwapchainImageViews();
  void createScaffoldDepthResources();
  void createScaffoldRenderPass();
  void createScaffoldPipelineLayout();
  void createUiTextureResources();
  void createScaffoldGraphicsPipeline();
  void createUiGraphicsPipeline();
  void createScaffoldFramebuffers();
  void createFrameScaffoldResources();

  void recreateSwapchain();
  void cleanupSwapchain();
  void cleanupScaffoldDepthResources();
  void cleanupScaffoldFramebuffers();
  void cleanupScaffoldGraphicsPipeline();
  void cleanupUiGraphicsPipeline();
  void cleanupUiTextureResources();
  void cleanupScaffoldPipelineLayout();
  void cleanupScaffoldRenderPass();
  void cleanupFrameScaffoldResources();

  [[nodiscard]] bool beginFrameScaffold();
  void recordScenePassScaffold();
  void recordUiPassScaffold();
  void endFrameScaffold();

  void beginScaffoldRenderPass(VkCommandBuffer commandBuffer) const;
  void endScaffoldRenderPass(VkCommandBuffer commandBuffer) const;
  void recordScaffoldViewportState(VkCommandBuffer commandBuffer) const;
  void recordScaffoldPipelineState(VkCommandBuffer commandBuffer) const;
  void recordScaffoldScenePlaceholderDraw(
      VkCommandBuffer commandBuffer,
      const std::optional<SceneSubmission>& submission) const;

  void createScaffoldCommandPool();
  void allocateScaffoldCommandBuffers();
  void recreateScaffoldCommandBuffers();
  void createScaffoldSyncObjects();
  void destroyScaffoldCommandBuffers();
  void updateScaffoldCameraMatrices();
  void ensureUiScaffoldBufferCapacity(std::size_t vertexBytes, std::size_t indexBytes);
  void createUiScaffoldBuffers(std::size_t vertexBytes, std::size_t indexBytes);
  void destroyUiScaffoldBuffers();
  void uploadUiMeshData(const volt::ui::UiMeshData& meshData);
  void recordUiMeshDraws(VkCommandBuffer commandBuffer, const volt::ui::UiMeshData& meshData);
  void createUiTextureResourceForKey(const std::string& textureKey);
  void destroyUiTextureResource(const std::string& textureKey);
  [[nodiscard]] VkDescriptorSet resolveUiDescriptorSetForBatch(const std::string& textureKey);

  struct FrameSync {
    VkSemaphore imageAvailable{VK_NULL_HANDLE};
    VkSemaphore renderFinished{VK_NULL_HANDLE};
    VkFence inFlight{VK_NULL_HANDLE};
  };

  [[nodiscard]] QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device) const;
  [[nodiscard]] SwapchainSupportDetails querySwapchainSupport(VkPhysicalDevice device) const;

  [[nodiscard]] bool isDeviceSuitable(VkPhysicalDevice device) const;

  [[nodiscard]] VkSurfaceFormatKHR chooseSwapSurfaceFormat(
      const std::vector<VkSurfaceFormatKHR>& availableFormats) const;
  [[nodiscard]] VkPresentModeKHR chooseSwapPresentMode(
      const std::vector<VkPresentModeKHR>& availablePresentModes) const;
  [[nodiscard]] VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities) const;
    [[nodiscard]] VkFormat findSupportedFormat(
      const std::vector<VkFormat>& candidates,
      VkImageTiling tiling,
      VkFormatFeatureFlags features) const;
    [[nodiscard]] VkFormat findDepthFormat() const;
    [[nodiscard]] std::uint32_t findMemoryType(
      std::uint32_t typeFilter,
      VkMemoryPropertyFlags properties) const;
    [[nodiscard]] VkShaderModule createShaderModule(const std::vector<char>& code) const;

  GLFWwindow* window_{nullptr};
  VkInstance instance_{VK_NULL_HANDLE};
  VkDebugUtilsMessengerEXT debugMessenger_{VK_NULL_HANDLE};
  VkSurfaceKHR surface_{VK_NULL_HANDLE};
  VkPhysicalDevice physicalDevice_{VK_NULL_HANDLE};
  VkDevice device_{VK_NULL_HANDLE};
  VkQueue graphicsQueue_{VK_NULL_HANDLE};
  VkQueue presentQueue_{VK_NULL_HANDLE};

  VkSwapchainKHR swapchain_{VK_NULL_HANDLE};
  std::vector<VkImage> swapchainImages_;
  std::vector<VkImageView> swapchainImageViews_;
  std::vector<VkFramebuffer> scaffoldFramebuffers_;
  VkImage scaffoldDepthImage_{VK_NULL_HANDLE};
  VkDeviceMemory scaffoldDepthImageMemory_{VK_NULL_HANDLE};
  VkImageView scaffoldDepthImageView_{VK_NULL_HANDLE};
  VkFormat swapchainImageFormat_{VK_FORMAT_UNDEFINED};
  VkFormat scaffoldDepthFormat_{VK_FORMAT_UNDEFINED};
  VkExtent2D swapchainExtent_{};
  VkRenderPass scaffoldRenderPass_{VK_NULL_HANDLE};
  VkPipelineLayout scaffoldPipelineLayout_{VK_NULL_HANDLE};
  VkDescriptorSetLayout uiDescriptorSetLayout_{VK_NULL_HANDLE};
  VkPipeline scaffoldGraphicsPipeline_{VK_NULL_HANDLE};
  VkPipeline uiGraphicsPipeline_{VK_NULL_HANDLE};
  VkDescriptorPool uiDescriptorPool_{VK_NULL_HANDLE};
  VkDescriptorSet uiDescriptorSet_{VK_NULL_HANDLE};
  struct UiTextureResource {
    VkImage image{VK_NULL_HANDLE};
    VkDeviceMemory imageMemory{VK_NULL_HANDLE};
    VkImageView imageView{VK_NULL_HANDLE};
    VkDescriptorSet descriptorSet{VK_NULL_HANDLE};
  };
  std::unordered_map<std::string, UiTextureResource> uiTextureResources_;
  VkImage uiTextureImage_{VK_NULL_HANDLE};
  VkDeviceMemory uiTextureImageMemory_{VK_NULL_HANDLE};
  VkImageView uiTextureImageView_{VK_NULL_HANDLE};
  VkSampler uiTextureSampler_{VK_NULL_HANDLE};
  std::unordered_set<std::string> unresolvedTextureKeysLogged_;

  VkCommandPool scaffoldCommandPool_{VK_NULL_HANDLE};
  std::vector<VkCommandBuffer> scaffoldCommandBuffers_;
  static constexpr std::uint32_t kMaxFramesInFlight = 2;
  std::array<FrameSync, kMaxFramesInFlight> frameSync_{};
  std::uint32_t currentFrameSlot_{0};
  std::uint32_t acquiredImageIndex_{0};
  bool frameScaffoldActive_{false};

  std::optional<SceneSubmission> pendingSceneSubmission_;
  volt::math::Vec3f scaffoldCameraPosition_{0.0F, 0.0F, 3.0F};
  volt::math::Vec3f scaffoldCameraTarget_{0.0F, 0.0F, 0.0F};
  volt::math::Mat4f scaffoldViewMatrix_{};
  volt::math::Mat4f scaffoldProjectionMatrix_{};
  volt::math::Mat4f scaffoldViewProjectionMatrix_{};
  std::uint64_t frameIndex_{0};
  UiPassCallback uiPassCallback_{};
  UiMeshProvider uiMeshProvider_{};
  VkBuffer uiVertexBuffer_{VK_NULL_HANDLE};
  VkDeviceMemory uiVertexBufferMemory_{VK_NULL_HANDLE};
  VkBuffer uiIndexBuffer_{VK_NULL_HANDLE};
  VkDeviceMemory uiIndexBufferMemory_{VK_NULL_HANDLE};
  std::size_t uiVertexBufferCapacityBytes_{0};
  std::size_t uiIndexBufferCapacityBytes_{0};
  volt::event::EventDispatcher* eventDispatcher_{nullptr};
  std::uint64_t resizeListenerId_{0};
  bool resizeEventPending_{false};
  std::chrono::steady_clock::time_point lastResizeEventAt_{};
};

}  // namespace volt::render
