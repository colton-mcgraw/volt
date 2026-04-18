#pragma once

#include "volt/render/Renderer.hpp"
#include "volt/render/details/VectorTextWorker.hpp"
#include "volt/math/Matrix.hpp"
#include "volt/math/Vector.hpp"
#include "volt/ui/UIRenderTypes.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <string>
#include <unordered_set>
#include <memory>
#include <vector>
#include <vulkan/vulkan.h>

namespace volt::platform {
class Window;
}

namespace volt::event {
class EventDispatcher;
}

namespace volt::ui {
struct UiMeshData;
struct UiVectorTextBatch;
}

namespace volt::render::details {

class VulkanRenderer final : public Renderer {
 public:
  VulkanRenderer(volt::platform::Window& window, const char* appName);
  ~VulkanRenderer();

  VulkanRenderer(const VulkanRenderer&) = delete;
  VulkanRenderer& operator=(const VulkanRenderer&) = delete;

  using UiPassCallback = std::function<void(VkCommandBuffer, std::uint32_t, std::uint32_t)>;

  void submitScene(const SceneSubmission& submission) override;
  void setUiPassCallback(UiPassCallback callback);
  void setUiMeshProvider(UiMeshProvider provider) override;
  void setEventDispatcher(volt::event::EventDispatcher* dispatcher) override;
  void tick(bool framebufferResized) override;
  [[nodiscard]] VectorTextGpuTimings vectorTextGpuTimings() const override;
  [[nodiscard]] FrameCpuTimings frameCpuTimings() const override;

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

  struct UiTextureResource;

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
  void createUiOffscreenRenderPass();
  void createScaffoldPipelineLayout();
  void createUiTextureResources();
  void createUiFontComputeResources();
  void createUiVectorTextComputeResources();
  void createScaffoldGraphicsPipeline();
  void createUiGraphicsPipeline();
  void createUiOpaqueGraphicsPipeline();
  void createUiTextSdfGraphicsPipeline();
  void createUiOffscreenGraphicsPipeline();
  void createUiOffscreenTextSdfGraphicsPipeline();
  void createScaffoldFramebuffers();
  void createFrameScaffoldResources();

  void recreateSwapchain();
  void cleanupSwapchain();
  void cleanupScaffoldDepthResources();
  void cleanupScaffoldFramebuffers();
  void cleanupScaffoldGraphicsPipeline();
  void cleanupUiGraphicsPipeline();
  void cleanupUiOpaqueGraphicsPipeline();
  void cleanupUiTextSdfGraphicsPipeline();
  void cleanupUiOffscreenGraphicsPipeline();
  void cleanupUiOffscreenTextSdfGraphicsPipeline();
  void cleanupUiTextureResources();
  void cleanupUiFontComputeResources();
  void cleanupUiVectorTextComputeResources();
  void cleanupScaffoldPipelineLayout();
  void cleanupScaffoldRenderPass();
  void cleanupUiOffscreenRenderPass();
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
  void destroyUiScaffoldBuffers(bool deferDestruction = false);
  void uploadUiMeshData(const volt::ui::UiMeshData& meshData);
  void recordUiMeshDraws(VkCommandBuffer commandBuffer, const volt::ui::UiMeshData& meshData);
  void recordUiMeshDrawsForExtent(
      VkCommandBuffer commandBuffer,
      const volt::ui::UiMeshData& meshData,
      std::uint32_t targetWidth,
      std::uint32_t targetHeight,
      float uiWidth,
      float uiHeight,
      float scaleX,
      float scaleY,
      VkPipeline solidPipeline,
      VkPipeline opaqueSolidPipeline,
      VkPipeline textPipeline,
      bool emitLogs,
      bool updateFrameStats);
  void prepareUiRetainedPanels(const volt::ui::UiMeshData& meshData);
  void prepareUiVectorTextBatches(const volt::ui::UiMeshData& meshData);
  void cleanupUiVectorTextTransientResources();
  void createUiTextureResourceForKey(const std::string& textureKey);
  void destroyUiTextureResource(const std::string& textureKey, bool deferDestruction = true);
  void releaseUiTextureResource(UiTextureResource& resource);
  void releaseRetiredUiTextureResources(std::uint32_t frameSlot);
  void retireBufferResource(VkBuffer& buffer, VkDeviceMemory& memory, const char* label = nullptr);
  void releaseRetiredBufferResources(std::uint32_t frameSlot);
  [[nodiscard]] bool renderUiVectorTextBatchToTexture(const volt::ui::UiVectorTextBatch& batch, std::string& outError);
  [[nodiscard]] bool renderUiRetainedPanelToTexture(const volt::ui::UiRetainedPanel& panel, std::string& outError);
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

  volt::platform::Window* window_{nullptr};
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
  VkRenderPass uiOffscreenRenderPass_{VK_NULL_HANDLE};
  VkPipelineLayout scaffoldPipelineLayout_{VK_NULL_HANDLE};
  VkDescriptorSetLayout uiDescriptorSetLayout_{VK_NULL_HANDLE};
  VkDescriptorSetLayout uiFontComputeDescriptorSetLayout_{VK_NULL_HANDLE};
  VkDescriptorSetLayout uiVectorTextComputeDescriptorSetLayout_{VK_NULL_HANDLE};
  VkDescriptorSetLayout uiVectorTextPrefixScanDescriptorSetLayout_{VK_NULL_HANDLE};
  VkPipelineLayout uiVectorTextPrefixScanPipelineLayout_{VK_NULL_HANDLE};
  VkDescriptorSetLayout uiVectorTextExclusiveScanDescriptorSetLayout_{VK_NULL_HANDLE};
  VkPipelineLayout uiVectorTextExclusiveScanPipelineLayout_{VK_NULL_HANDLE};
  VkPipeline uiVectorTextExclusiveScanPipeline_{VK_NULL_HANDLE};
  VkPipeline scaffoldGraphicsPipeline_{VK_NULL_HANDLE};
  VkPipeline uiGraphicsPipeline_{VK_NULL_HANDLE};
  VkPipeline uiOpaqueGraphicsPipeline_{VK_NULL_HANDLE};
  VkPipeline uiTextSdfGraphicsPipeline_{VK_NULL_HANDLE};
  VkPipeline uiOffscreenGraphicsPipeline_{VK_NULL_HANDLE};
  VkPipeline uiOffscreenTextSdfGraphicsPipeline_{VK_NULL_HANDLE};
  VkPipeline uiFontComputePipeline_{VK_NULL_HANDLE};
  VkPipeline uiVectorTextFlattenCountPipeline_{VK_NULL_HANDLE};
  VkPipeline uiVectorTextFlattenEmitPipeline_{VK_NULL_HANDLE};
  VkPipeline uiVectorTextBinCountPipeline_{VK_NULL_HANDLE};
  VkPipeline uiVectorTextBinEmitPipeline_{VK_NULL_HANDLE};
  VkPipeline uiVectorTextFinePipeline_{VK_NULL_HANDLE};
  VkPipeline uiVectorTextPrefixScanPipeline_{VK_NULL_HANDLE};
  VkDescriptorPool uiDescriptorPool_{VK_NULL_HANDLE};
  VkDescriptorPool uiFontComputeDescriptorPool_{VK_NULL_HANDLE};
  VkDescriptorPool uiVectorTextComputeDescriptorPool_{VK_NULL_HANDLE};
  VkDescriptorSet uiDescriptorSet_{VK_NULL_HANDLE};
  VkPipelineLayout uiFontComputePipelineLayout_{VK_NULL_HANDLE};
  VkPipelineLayout uiVectorTextComputePipelineLayout_{VK_NULL_HANDLE};
  struct UiTextureResource {
    VkImage image{VK_NULL_HANDLE};
    VkDeviceMemory imageMemory{VK_NULL_HANDLE};
    VkImageView imageView{VK_NULL_HANDLE};
    VkFramebuffer framebuffer{VK_NULL_HANDLE};
    VkDescriptorSet descriptorSet{VK_NULL_HANDLE};
    std::uint64_t fontAtlasRevision{0U};
    std::uint64_t vectorTextSignature{0U};
    std::uint64_t retainedPanelSignature{0U};
    std::uint32_t width{0U};
    std::uint32_t height{0U};
  };
  std::unordered_map<std::string, UiTextureResource> uiTextureResources_;
  static constexpr std::uint32_t kMaxFramesInFlight = 3;
  std::array<std::vector<UiTextureResource>, kMaxFramesInFlight> retiredUiTextureResources_{};
  struct RetiredBufferResource {
    VkBuffer buffer{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    const char* label{nullptr};
  };
  std::array<std::vector<RetiredBufferResource>, kMaxFramesInFlight> retiredBufferResources_{};
  VkImage uiTextureImage_{VK_NULL_HANDLE};
  VkDeviceMemory uiTextureImageMemory_{VK_NULL_HANDLE};
  VkImageView uiTextureImageView_{VK_NULL_HANDLE};
  VkSampler uiTextureSampler_{VK_NULL_HANDLE};
  std::unordered_set<std::string> unresolvedTextureKeysLogged_;
  std::unordered_set<std::string> uiVectorTextTransientKeys_;

  struct UiSdfPushConstants {
    volt::math::Mat4f uiTransform{volt::math::Mat4f::identity()};
    float pxRange{0.0F};
    float edge{0.5F};
    float aaStrength{0.35F};
    float msdfMode{0.0F};
    float msdfConfidenceLow{0.01F};
    float msdfConfidenceHigh{0.07F};
    float subpixelBlendStrength{0.85F};
    float smallTextSharpenStrength{0.28F};
  };

  struct UiVectorTextPushConstants {
    std::uint32_t curveCount{0};
    std::uint32_t segmentCount{0};
    std::uint32_t imageWidth{0};
    std::uint32_t imageHeight{0};
    std::uint32_t tilesWide{0};
    std::uint32_t tilesHigh{0};
    float flatnessThresholdPx{0.25F};
    float pad0{0.0F};
    volt::ui::Color color{};
  };

  VkCommandPool scaffoldCommandPool_{VK_NULL_HANDLE};
  std::vector<VkCommandBuffer> scaffoldCommandBuffers_;
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
  void* uiVertexBufferMapped_{nullptr};
  VkBuffer uiIndexBuffer_{VK_NULL_HANDLE};
  VkDeviceMemory uiIndexBufferMemory_{VK_NULL_HANDLE};
  void* uiIndexBufferMapped_{nullptr};
  std::size_t uiVertexBufferCapacityBytes_{0};
  std::size_t uiIndexBufferCapacityBytes_{0};
  volt::event::EventDispatcher* eventDispatcher_{nullptr};
  std::uint64_t resizeListenerId_{0};
  bool resizeEventPending_{false};
  std::chrono::steady_clock::time_point lastResizeEventAt_{};
  std::chrono::steady_clock::time_point lastFrameStallLogAt_{};
  VectorTextGpuTimings latestVectorTextGpuTimings_{};
  FrameCpuTimings latestFrameCpuTimings_{};
  
  std::unique_ptr<VectorTextWorker> vectorTextWorker_{};
};

}  // namespace volt::render::details
