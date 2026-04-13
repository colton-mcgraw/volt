#include "volt/render/VulkanRenderer.hpp"

#include "volt/core/Logging.hpp"
#include "volt/event/Event.hpp"
#include "volt/event/EventDispatcher.hpp"
#include "volt/io/assets/AssetManager.hpp"
#include "volt/math/Math.hpp"
#include "volt/ui/UIMesh.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace volt::render
{

  namespace
  {
    constexpr std::uint32_t kRendererVersionMajor = 0;
    constexpr std::uint32_t kRendererVersionMinor = 1;
    constexpr std::uint32_t kRendererVersionPatch = 0;

#if defined(VOLT_ENABLE_DEBUG_LOGGING) && VOLT_ENABLE_DEBUG_LOGGING
    constexpr const char* kValidationLayerName = "VK_LAYER_KHRONOS_validation";

    VKAPI_ATTR VkBool32 VKAPI_CALL debugUtilsCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
        VkDebugUtilsMessageTypeFlagsEXT messageTypes,
        const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
        void* userData)
    {
      (void)messageTypes;
      (void)userData;

      if (callbackData == nullptr || callbackData->pMessage == nullptr) {
        return VK_FALSE;
      }

      const char* message = callbackData->pMessage;
      if ((messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0U) {
        VOLT_LOG_ERROR_CAT(volt::core::logging::Category::kRender, "[Vulkan] ", message);
      } else if ((messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0U) {
        VOLT_LOG_WARN_CAT(volt::core::logging::Category::kRender, "[Vulkan] ", message);
      } else if ((messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) != 0U) {
        VOLT_LOG_INFO_CAT(volt::core::logging::Category::kRender, "[Vulkan] ", message);
      } else {
        VOLT_LOG_DEBUG_CAT(volt::core::logging::Category::kRender, "[Vulkan] ", message);
      }

      return VK_FALSE;
    }

    bool hasInstanceExtension(const char* extensionName)
    {
      uint32_t extensionCount = 0;
      vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);
      if (extensionCount == 0) {
        return false;
      }

      std::vector<VkExtensionProperties> extensions(extensionCount);
      vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, extensions.data());
      for (const VkExtensionProperties& extension : extensions) {
        if (std::strcmp(extension.extensionName, extensionName) == 0) {
          return true;
        }
      }

      return false;
    }

    bool hasValidationLayer(const char* layerName)
    {
      uint32_t layerCount = 0;
      vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
      if (layerCount == 0) {
        return false;
      }

      std::vector<VkLayerProperties> layers(layerCount);
      vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
      for (const VkLayerProperties& layer : layers) {
        if (std::strcmp(layer.layerName, layerName) == 0) {
          return true;
        }
      }

      return false;
    }
#endif

    std::optional<std::vector<char>> readBinaryFile(const std::string& path)
    {
      std::ifstream file(path, std::ios::ate | std::ios::binary);
      if (!file.is_open()) {
        return std::nullopt;
      }

      const std::streamsize fileSize = file.tellg();
      if (fileSize <= 0) {
        return std::nullopt;
      }

      std::vector<char> buffer(static_cast<std::size_t>(fileSize));
      file.seekg(0);
      file.read(buffer.data(), fileSize);

      if (!file.good()) {
        return std::nullopt;
      }

      return buffer;
    }

  } // namespace

  bool VulkanRenderer::QueueFamilyIndices::isComplete() const
  {
    return graphicsFamily.has_value() && presentFamily.has_value();
  }

  VulkanRenderer::VulkanRenderer(GLFWwindow *window, const char *appName) : window_(window)
  {
    if (window_ == nullptr)
    {
      throw std::invalid_argument("VulkanRenderer requires a valid window handle");
    }

    createInstance(appName);
    setupDebugMessenger();
    createSurface();
    pickPhysicalDevice();
    createLogicalDevice();
    createSwapchain();
    createSwapchainImageViews();
    createScaffoldDepthResources();
    createScaffoldRenderPass();
    createScaffoldPipelineLayout();
    createScaffoldGraphicsPipeline();
    createUiGraphicsPipeline();
    createScaffoldFramebuffers();
    createFrameScaffoldResources();
    createUiTextureResources();
    VOLT_LOG_INFO_CAT(volt::core::logging::Category::kRender, "Renderer initialized for app: ", appName);
  }

  VulkanRenderer::~VulkanRenderer()
  {
    if (device_ != VK_NULL_HANDLE)
    {
      vkDeviceWaitIdle(device_);
    }

    cleanupFrameScaffoldResources();
    destroyUiScaffoldBuffers();
    cleanupUiTextureResources();
    cleanupSwapchain();
    cleanupUiGraphicsPipeline();
    cleanupScaffoldGraphicsPipeline();
    cleanupScaffoldPipelineLayout();
    cleanupScaffoldRenderPass();

    if (device_ != VK_NULL_HANDLE)
    {
      vkDestroyDevice(device_, nullptr);
      device_ = VK_NULL_HANDLE;
    }

    if (surface_ != VK_NULL_HANDLE)
    {
      vkDestroySurfaceKHR(instance_, surface_, nullptr);
      surface_ = VK_NULL_HANDLE;
    }

    cleanupDebugMessenger();

    if (instance_ != VK_NULL_HANDLE)
    {
      vkDestroyInstance(instance_, nullptr);
      instance_ = VK_NULL_HANDLE;
    }
  }

  void VulkanRenderer::submitScene(const SceneSubmission &submission)
  {
    pendingSceneSubmission_ = submission;
  }

  void VulkanRenderer::setUiPassCallback(UiPassCallback callback)
  {
    uiPassCallback_ = std::move(callback);
  }

  void VulkanRenderer::setUiMeshProvider(UiMeshProvider provider)
  {
    uiMeshProvider_ = std::move(provider);
  }

  void VulkanRenderer::setEventDispatcher(volt::event::EventDispatcher* dispatcher)
  {
    if (eventDispatcher_ != nullptr && resizeListenerId_ != 0) {
      eventDispatcher_->unsubscribe(resizeListenerId_);
      resizeListenerId_ = 0;
    }

    eventDispatcher_ = dispatcher;

    if (eventDispatcher_ != nullptr) {
      resizeListenerId_ = eventDispatcher_->subscribe(
          volt::event::EventType::kWindowResized,
          [this](const volt::event::Event& event) {
            const auto* resizePayload = std::get_if<volt::event::WindowResizeEvent>(&event.payload);
            if (resizePayload == nullptr) {
              return;
            }

            (void)resizePayload;
            resizeEventPending_ = true;
            lastResizeEventAt_ = std::chrono::steady_clock::now();
          });
    }
  }

  void VulkanRenderer::tick(bool framebufferResized)
  {
    if (swapchain_ == VK_NULL_HANDLE)
    {
      return;
    }

    constexpr auto kResizeRecreateDebounce = std::chrono::milliseconds(75);
    const auto tickNow = std::chrono::steady_clock::now();

    const bool effectiveFramebufferResized = framebufferResized || resizeEventPending_;
    const bool resizeDebounceElapsed =
        (lastResizeEventAt_ == std::chrono::steady_clock::time_point{}) ||
        (tickNow - lastResizeEventAt_ >= kResizeRecreateDebounce);

    if (effectiveFramebufferResized)
    {
      if (resizeDebounceElapsed)
      {
        resizeEventPending_ = false;
        recreateSwapchain();
      }
    }

    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window_, &width, &height);
    if (width == 0 || height == 0)
    {
      return;
    }

    if (static_cast<std::uint32_t>(width) != swapchainExtent_.width ||
        static_cast<std::uint32_t>(height) != swapchainExtent_.height)
    {
      if (resizeDebounceElapsed)
      {
        recreateSwapchain();
        glfwGetFramebufferSize(window_, &width, &height);
        if (width == 0 || height == 0)
        {
          return;
        }
      }
    }

    updateScaffoldCameraMatrices();

    if (!beginFrameScaffold())
    {
      return;
    }
    recordScenePassScaffold();
    recordUiPassScaffold();
    endFrameScaffold();
  }

  bool VulkanRenderer::beginFrameScaffold()
  {
    FrameSync& sync = frameSync_[currentFrameSlot_];
    if (sync.inFlight != VK_NULL_HANDLE) {
      const VkResult waitResult = vkWaitForFences(device_, 1, &sync.inFlight, VK_TRUE, UINT64_MAX);
      if (waitResult != VK_SUCCESS) {
        throw std::runtime_error("Failed to wait for frame scaffold fence");
      }

      const VkResult resetResult = vkResetFences(device_, 1, &sync.inFlight);
      if (resetResult != VK_SUCCESS) {
        throw std::runtime_error("Failed to reset frame scaffold fence");
      }
    }

    const VkResult acquireResult = vkAcquireNextImageKHR(
        device_,
        swapchain_,
        UINT64_MAX,
        sync.imageAvailable,
        VK_NULL_HANDLE,
        &acquiredImageIndex_);

    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
      recreateSwapchain();
      frameScaffoldActive_ = false;
      return false;
    }

    if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) {
      throw std::runtime_error("Failed to acquire swapchain image");
    }

    if (acquiredImageIndex_ >= scaffoldCommandBuffers_.size()) {
      throw std::runtime_error("Swapchain image index out of range for scaffold command buffers");
    }

    VkCommandBuffer commandBuffer = scaffoldCommandBuffers_[acquiredImageIndex_];

    const VkResult resetCommandBufferResult = vkResetCommandBuffer(commandBuffer, 0);
    if (resetCommandBufferResult != VK_SUCCESS) {
      throw std::runtime_error("Failed to reset scaffold command buffer");
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
      throw std::runtime_error("Failed to begin scaffold command buffer");
    }

    frameScaffoldActive_ = true;
    ++frameIndex_;

    if (eventDispatcher_ != nullptr) {
      eventDispatcher_->enqueue({
          .type = volt::event::EventType::kRenderFrameBegin,
          .payload = volt::event::RenderStageEvent{
              .stage = volt::event::RenderStage::kFrameBegin,
              .frameIndex = frameIndex_,
          },
      });
    }

    return true;
  }

  void VulkanRenderer::recordScenePassScaffold()
  {
    if (!frameScaffoldActive_) {
      return;
    }

    if (acquiredImageIndex_ >= scaffoldFramebuffers_.size()) {
      throw std::runtime_error("Swapchain image index out of range for scaffold framebuffers");
    }

    VkCommandBuffer commandBuffer = scaffoldCommandBuffers_[acquiredImageIndex_];

    if (eventDispatcher_ != nullptr) {
      eventDispatcher_->enqueue({
        .type = volt::event::EventType::kRenderScenePassBegin,
        .payload = volt::event::RenderStageEvent{
          .stage = volt::event::RenderStage::kScenePassBegin,
          .frameIndex = frameIndex_,
        },
      });
    }

    beginScaffoldRenderPass(commandBuffer);
    recordScaffoldViewportState(commandBuffer);
    recordScaffoldPipelineState(commandBuffer);
    recordScaffoldScenePlaceholderDraw(commandBuffer, pendingSceneSubmission_);

    if (eventDispatcher_ != nullptr) {
      eventDispatcher_->enqueue({
        .type = volt::event::EventType::kRenderScenePassEnd,
        .payload = volt::event::RenderStageEvent{
          .stage = volt::event::RenderStage::kScenePassEnd,
          .frameIndex = frameIndex_,
        },
      });
    }

    pendingSceneSubmission_.reset();
  }

  void VulkanRenderer::beginScaffoldRenderPass(VkCommandBuffer commandBuffer) const
  {
    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color = {{0.07F, 0.08F, 0.12F, 1.0F}};
    clearValues[1].depthStencil = {1.0F, 0U};

    VkRenderPassBeginInfo renderPassBeginInfo{};
    renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassBeginInfo.renderPass = scaffoldRenderPass_;
    renderPassBeginInfo.framebuffer = scaffoldFramebuffers_[acquiredImageIndex_];
    renderPassBeginInfo.renderArea.offset = {0, 0};
    renderPassBeginInfo.renderArea.extent = swapchainExtent_;
    renderPassBeginInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassBeginInfo.pClearValues = clearValues.data();

    vkCmdBeginRenderPass(commandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
  }

  void VulkanRenderer::endScaffoldRenderPass(VkCommandBuffer commandBuffer) const
  {
    vkCmdEndRenderPass(commandBuffer);
  }

  void VulkanRenderer::recordScaffoldViewportState(VkCommandBuffer commandBuffer) const
  {
    VkViewport viewport{};
    viewport.x = 0.0F;
    viewport.y = 0.0F;
    viewport.width = static_cast<float>(swapchainExtent_.width);
    viewport.height = static_cast<float>(swapchainExtent_.height);
    viewport.minDepth = 0.0F;
    viewport.maxDepth = 1.0F;

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = swapchainExtent_;

    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
  }

  void VulkanRenderer::recordScaffoldPipelineState(VkCommandBuffer commandBuffer) const
  {
    if (scaffoldGraphicsPipeline_ == VK_NULL_HANDLE) {
      return;
    }

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, scaffoldGraphicsPipeline_);
  }

  void VulkanRenderer::recordScaffoldScenePlaceholderDraw(
      VkCommandBuffer commandBuffer,
      const std::optional<SceneSubmission>& submission) const
  {
    // Placeholder hook for mesh draw recording. Real pipeline/buffer bindings will replace this.
    if (!submission.has_value() || scaffoldGraphicsPipeline_ == VK_NULL_HANDLE) {
      return;
    }

    const std::uint32_t meshCount = submission->meshCount;
    const std::uint32_t instanceCount = submission->instanceCount;

    (void)meshCount;
    (void)instanceCount;
    (void)commandBuffer;
  }

  void VulkanRenderer::recordUiPassScaffold()
  {
    if (uiMeshProvider_) {
      const volt::ui::UiMeshData* meshData = uiMeshProvider_();
      if (meshData != nullptr && !meshData->vertices.empty() && !meshData->indices.empty()) {
        uploadUiMeshData(*meshData);
        if (acquiredImageIndex_ < scaffoldCommandBuffers_.size()) {
          recordUiMeshDraws(scaffoldCommandBuffers_[acquiredImageIndex_], *meshData);
        }
      }
    }

    if (uiPassCallback_ && acquiredImageIndex_ < scaffoldCommandBuffers_.size()) {
      uiPassCallback_(
          scaffoldCommandBuffers_[acquiredImageIndex_],
          swapchainExtent_.width,
          swapchainExtent_.height);
    }

    if (eventDispatcher_ != nullptr) {
      eventDispatcher_->enqueue({
          .type = volt::event::EventType::kRenderUiPass,
          .payload = volt::event::RenderStageEvent{
              .stage = volt::event::RenderStage::kUiPass,
              .frameIndex = frameIndex_,
          },
      });
    }
  }

  void VulkanRenderer::endFrameScaffold()
  {
    if (!frameScaffoldActive_) {
      return;
    }

    FrameSync& sync = frameSync_[currentFrameSlot_];

    VkCommandBuffer commandBuffer = scaffoldCommandBuffers_[acquiredImageIndex_];
    endScaffoldRenderPass(commandBuffer);
    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
      throw std::runtime_error("Failed to end scaffold command buffer");
    }

    constexpr VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &sync.imageAvailable;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &sync.renderFinished;

    const VkResult submitResult = vkQueueSubmit(graphicsQueue_, 1, &submitInfo, sync.inFlight);
    if (submitResult != VK_SUCCESS) {
      throw std::runtime_error("Failed to submit frame scaffold synchronization work");
    }

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &sync.renderFinished;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain_;
    presentInfo.pImageIndices = &acquiredImageIndex_;

    const VkResult presentResult = vkQueuePresentKHR(presentQueue_, &presentInfo);
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
      recreateSwapchain();
    } else if (presentResult != VK_SUCCESS) {
      throw std::runtime_error("Failed to present swapchain image");
    }

    currentFrameSlot_ = (currentFrameSlot_ + 1U) % kMaxFramesInFlight;
    frameScaffoldActive_ = false;

    if (eventDispatcher_ != nullptr) {
      eventDispatcher_->enqueue({
          .type = volt::event::EventType::kRenderFrameEnd,
          .payload = volt::event::RenderStageEvent{
              .stage = volt::event::RenderStage::kFrameEnd,
              .frameIndex = frameIndex_,
          },
      });
    }
  }

  void VulkanRenderer::createFrameScaffoldResources()
  {
    createScaffoldCommandPool();
    allocateScaffoldCommandBuffers();
    createScaffoldSyncObjects();
    updateScaffoldCameraMatrices();
  }

  void VulkanRenderer::updateScaffoldCameraMatrices()
  {
    if (swapchainExtent_.height == 0) {
      return;
    }

    const float aspectRatio =
        static_cast<float>(swapchainExtent_.width) / static_cast<float>(swapchainExtent_.height);
    scaffoldViewMatrix_ = volt::math::lookAtRH(
        scaffoldCameraPosition_,
        scaffoldCameraTarget_,
        volt::math::Vec3f{0.0F, 1.0F, 0.0F});
    scaffoldProjectionMatrix_ = volt::math::perspectiveRH(
        volt::math::radians(60.0F),
        aspectRatio,
        0.1F,
        1000.0F,
        volt::math::ClipSpaceZ::kZeroToOne);
    scaffoldViewProjectionMatrix_ = scaffoldProjectionMatrix_ * scaffoldViewMatrix_;

    VOLT_LOG_TRACE_CAT(
        volt::core::logging::Category::kRender,
        "Scaffold VP[0,0]=",
        scaffoldViewProjectionMatrix_.at(0, 0),
        " VP[2,2]=",
        scaffoldViewProjectionMatrix_.at(2, 2));
  }

  void VulkanRenderer::createScaffoldRenderPass()
  {
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = swapchainImageFormat_;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorAttachmentRef{};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = scaffoldDepthFormat_;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthAttachmentRef{};
    depthAttachmentRef.attachment = 1;
    depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorAttachmentRef;
    subpass.pDepthStencilAttachment = &depthAttachmentRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask =
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstStageMask =
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask =
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    const std::array<VkAttachmentDescription, 2> attachments = {colorAttachment, depthAttachment};

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    if (vkCreateRenderPass(device_, &renderPassInfo, nullptr, &scaffoldRenderPass_) != VK_SUCCESS) {
      throw std::runtime_error("Failed to create scaffold render pass");
    }
  }

  void VulkanRenderer::createScaffoldPipelineLayout()
  {
    VkDescriptorSetLayoutBinding textureBinding{};
    textureBinding.binding = 0;
    textureBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    textureBinding.descriptorCount = 1;
    textureBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo descriptorSetLayoutInfo{};
    descriptorSetLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptorSetLayoutInfo.bindingCount = 1;
    descriptorSetLayoutInfo.pBindings = &textureBinding;

    if (vkCreateDescriptorSetLayout(device_, &descriptorSetLayoutInfo, nullptr, &uiDescriptorSetLayout_) != VK_SUCCESS) {
      throw std::runtime_error("Failed to create UI descriptor set layout");
    }

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &uiDescriptorSetLayout_;

    if (vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &scaffoldPipelineLayout_) != VK_SUCCESS) {
      throw std::runtime_error("Failed to create scaffold pipeline layout");
    }

    scaffoldGraphicsPipeline_ = VK_NULL_HANDLE;
  }

  void VulkanRenderer::createUiTextureResources()
  {
    if (device_ == VK_NULL_HANDLE || scaffoldCommandPool_ == VK_NULL_HANDLE) {
      throw std::runtime_error("Cannot create UI texture resources before frame scaffold resources");
    }

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0F;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_WHITE;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

    if (vkCreateSampler(device_, &samplerInfo, nullptr, &uiTextureSampler_) != VK_SUCCESS) {
      throw std::runtime_error("Failed to create UI texture sampler");
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 256;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = 256;

    if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &uiDescriptorPool_) != VK_SUCCESS) {
      throw std::runtime_error("Failed to create UI descriptor pool");
    }

    createUiTextureResourceForKey("__white");
    const auto whiteIt = uiTextureResources_.find("__white");
    if (whiteIt != uiTextureResources_.end()) {
      uiDescriptorSet_ = whiteIt->second.descriptorSet;
      uiTextureImage_ = whiteIt->second.image;
      uiTextureImageMemory_ = whiteIt->second.imageMemory;
      uiTextureImageView_ = whiteIt->second.imageView;
    }
  }

  void VulkanRenderer::createUiTextureResourceForKey(const std::string& textureKey)
  {
    if (textureKey.empty() || uiTextureResources_.find(textureKey) != uiTextureResources_.end()) {
      return;
    }

    volt::io::LoadedImageAsset image = textureKey == "__white"
                         ? volt::io::LoadedImageAsset{1U, 1U, {255U, 255U, 255U, 255U}, false, {}}
                         : volt::io::AssetManager::instance().loadImage(textureKey);
    if (image.rgba.empty() || image.width == 0U || image.height == 0U) {
      image = volt::io::LoadedImageAsset{1U, 1U, {0U, 0U, 0U, 0U}, true, image.resolvedPath};
    }

    if (textureKey != "__white") {
      if (image.resolvedPath.empty()) {
        VOLT_LOG_WARN_CAT(
            volt::core::logging::Category::kRender,
            "UI image key unresolved, using placeholder: ",
            textureKey);
      } else {
        VOLT_LOG_DEBUG_CAT(
            volt::core::logging::Category::kRender,
            "UI image resolved: key=",
            textureKey,
            " path=",
            image.resolvedPath,
            " size=",
            image.width,
            "x",
            image.height,
            image.placeholder ? " (placeholder)" : "");
      }
    }

    const VkDeviceSize imageBytes = static_cast<VkDeviceSize>(image.rgba.size());

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingBufferMemory = VK_NULL_HANDLE;

    VkBufferCreateInfo stagingBufferInfo{};
    stagingBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    stagingBufferInfo.size = imageBytes;
    stagingBufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    stagingBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device_, &stagingBufferInfo, nullptr, &stagingBuffer) != VK_SUCCESS) {
      throw std::runtime_error("Failed to create UI keyed texture staging buffer");
    }

    VkMemoryRequirements stagingMemoryRequirements{};
    vkGetBufferMemoryRequirements(device_, stagingBuffer, &stagingMemoryRequirements);

    VkMemoryAllocateInfo stagingAllocInfo{};
    stagingAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    stagingAllocInfo.allocationSize = stagingMemoryRequirements.size;
    stagingAllocInfo.memoryTypeIndex = findMemoryType(
        stagingMemoryRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(device_, &stagingAllocInfo, nullptr, &stagingBufferMemory) != VK_SUCCESS) {
      throw std::runtime_error("Failed to allocate UI keyed texture staging memory");
    }

    if (vkBindBufferMemory(device_, stagingBuffer, stagingBufferMemory, 0) != VK_SUCCESS) {
      throw std::runtime_error("Failed to bind UI keyed texture staging memory");
    }

    void* mapped = nullptr;
    if (vkMapMemory(device_, stagingBufferMemory, 0, imageBytes, 0, &mapped) != VK_SUCCESS) {
      throw std::runtime_error("Failed to map UI keyed texture staging memory");
    }
    std::memcpy(mapped, image.rgba.data(), image.rgba.size());
    vkUnmapMemory(device_, stagingBufferMemory);

    UiTextureResource resource{};

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {image.width, image.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device_, &imageInfo, nullptr, &resource.image) != VK_SUCCESS) {
      throw std::runtime_error("Failed to create UI keyed texture image");
    }

    VkMemoryRequirements imageMemoryRequirements{};
    vkGetImageMemoryRequirements(device_, resource.image, &imageMemoryRequirements);

    VkMemoryAllocateInfo imageAllocInfo{};
    imageAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    imageAllocInfo.allocationSize = imageMemoryRequirements.size;
    imageAllocInfo.memoryTypeIndex = findMemoryType(
        imageMemoryRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(device_, &imageAllocInfo, nullptr, &resource.imageMemory) != VK_SUCCESS) {
      throw std::runtime_error("Failed to allocate UI keyed texture image memory");
    }

    if (vkBindImageMemory(device_, resource.image, resource.imageMemory, 0) != VK_SUCCESS) {
      throw std::runtime_error("Failed to bind UI keyed texture image memory");
    }

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = scaffoldCommandPool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device_, &allocInfo, &commandBuffer) != VK_SUCCESS) {
      throw std::runtime_error("Failed to allocate UI keyed texture command buffer");
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer, &beginInfo);

    VkImageMemoryBarrier toTransfer{};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = resource.image;
    toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toTransfer.subresourceRange.baseMipLevel = 0;
    toTransfer.subresourceRange.levelCount = 1;
    toTransfer.subresourceRange.baseArrayLayer = 0;
    toTransfer.subresourceRange.layerCount = 1;
    toTransfer.srcAccessMask = 0;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &toTransfer);

    VkBufferImageCopy copyRegion{};
    copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.imageSubresource.mipLevel = 0;
    copyRegion.imageSubresource.baseArrayLayer = 0;
    copyRegion.imageSubresource.layerCount = 1;
    copyRegion.imageExtent = {image.width, image.height, 1};

    vkCmdCopyBufferToImage(
        commandBuffer,
        stagingBuffer,
        resource.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &copyRegion);

    VkImageMemoryBarrier toSampled{};
    toSampled.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSampled.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toSampled.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toSampled.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSampled.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSampled.image = resource.image;
    toSampled.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toSampled.subresourceRange.baseMipLevel = 0;
    toSampled.subresourceRange.levelCount = 1;
    toSampled.subresourceRange.baseArrayLayer = 0;
    toSampled.subresourceRange.layerCount = 1;
    toSampled.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toSampled.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &toSampled);

    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    if (vkQueueSubmit(graphicsQueue_, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS) {
      throw std::runtime_error("Failed to submit UI keyed texture upload command buffer");
    }
    vkQueueWaitIdle(graphicsQueue_);

    vkFreeCommandBuffers(device_, scaffoldCommandPool_, 1, &commandBuffer);
    vkDestroyBuffer(device_, stagingBuffer, nullptr);
    vkFreeMemory(device_, stagingBufferMemory, nullptr);

    VkImageViewCreateInfo imageViewInfo{};
    imageViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    imageViewInfo.image = resource.image;
    imageViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    imageViewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageViewInfo.subresourceRange.baseMipLevel = 0;
    imageViewInfo.subresourceRange.levelCount = 1;
    imageViewInfo.subresourceRange.baseArrayLayer = 0;
    imageViewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device_, &imageViewInfo, nullptr, &resource.imageView) != VK_SUCCESS) {
      throw std::runtime_error("Failed to create UI keyed texture image view");
    }

    VkDescriptorSetAllocateInfo setAllocInfo{};
    setAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setAllocInfo.descriptorPool = uiDescriptorPool_;
    setAllocInfo.descriptorSetCount = 1;
    setAllocInfo.pSetLayouts = &uiDescriptorSetLayout_;

    if (vkAllocateDescriptorSets(device_, &setAllocInfo, &resource.descriptorSet) != VK_SUCCESS) {
      throw std::runtime_error("Failed to allocate UI keyed descriptor set");
    }

    VkDescriptorImageInfo imageDescriptorInfo{};
    imageDescriptorInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageDescriptorInfo.imageView = resource.imageView;
    imageDescriptorInfo.sampler = uiTextureSampler_;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = resource.descriptorSet;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &imageDescriptorInfo;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    uiTextureResources_.emplace(textureKey, resource);
  }

  void VulkanRenderer::destroyUiTextureResource(const std::string& textureKey)
  {
    const auto it = uiTextureResources_.find(textureKey);
    if (it == uiTextureResources_.end()) {
      return;
    }

    if (it->second.imageView != VK_NULL_HANDLE) {
      vkDestroyImageView(device_, it->second.imageView, nullptr);
    }
    if (it->second.image != VK_NULL_HANDLE) {
      vkDestroyImage(device_, it->second.image, nullptr);
    }
    if (it->second.imageMemory != VK_NULL_HANDLE) {
      vkFreeMemory(device_, it->second.imageMemory, nullptr);
    }

    uiTextureResources_.erase(it);
  }

  void VulkanRenderer::createScaffoldGraphicsPipeline()
  {
    if (scaffoldPipelineLayout_ == VK_NULL_HANDLE || scaffoldRenderPass_ == VK_NULL_HANDLE) {
      return;
    }

    const auto vertexShader = readBinaryFile("assets/shaders/scaffold.vert.spv");
    const auto fragmentShader = readBinaryFile("assets/shaders/scaffold.frag.spv");
    if (!vertexShader.has_value() || !fragmentShader.has_value()) {
      VOLT_LOG_ERROR_CAT(
          volt::core::logging::Category::kRender,
          "Missing scaffold shaders at assets/shaders/scaffold.vert.spv or scaffold.frag.spv");
      scaffoldGraphicsPipeline_ = VK_NULL_HANDLE;
      return;
    }

    VkShaderModule vertShaderModule = createShaderModule(vertexShader.value());
    VkShaderModule fragShaderModule = createShaderModule(fragmentShader.value());

    VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
    vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertShaderStageInfo.module = vertShaderModule;
    vertShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
    fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragShaderStageInfo.module = fragShaderModule;
    fragShaderStageInfo.pName = "main";

    const std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages = {
        vertShaderStageInfo,
        fragShaderStageInfo,
    };

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkVertexInputBindingDescription bindingDescription{};
    bindingDescription.binding = 0;
    bindingDescription.stride = static_cast<uint32_t>(sizeof(volt::ui::UiVertex));
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 3> attributeDescriptions{};
    attributeDescriptions[0].location = 0;
    attributeDescriptions[0].binding = 0;
    attributeDescriptions[0].format = VK_FORMAT_R32G32_SFLOAT;
    attributeDescriptions[0].offset = static_cast<uint32_t>(offsetof(volt::ui::UiVertex, x));

    attributeDescriptions[1].location = 1;
    attributeDescriptions[1].binding = 0;
    attributeDescriptions[1].format = VK_FORMAT_R32G32_SFLOAT;
    attributeDescriptions[1].offset = static_cast<uint32_t>(offsetof(volt::ui::UiVertex, u));

    attributeDescriptions[2].location = 2;
    attributeDescriptions[2].binding = 0;
    attributeDescriptions[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attributeDescriptions[2].offset = static_cast<uint32_t>(offsetof(volt::ui::UiVertex, color));

    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0F;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
        VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    constexpr std::array<VkDynamicState, 2> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };

    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
    pipelineInfo.pStages = shaderStages.data();
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = scaffoldPipelineLayout_;
    pipelineInfo.renderPass = scaffoldRenderPass_;
    pipelineInfo.subpass = 0;

    const VkResult pipelineResult = vkCreateGraphicsPipelines(
        device_,
        VK_NULL_HANDLE,
        1,
        &pipelineInfo,
        nullptr,
        &scaffoldGraphicsPipeline_);

    vkDestroyShaderModule(device_, fragShaderModule, nullptr);
    vkDestroyShaderModule(device_, vertShaderModule, nullptr);

    if (pipelineResult != VK_SUCCESS) {
      throw std::runtime_error("Failed to create scaffold graphics pipeline");
    }
  }

  void VulkanRenderer::createUiGraphicsPipeline()
  {
    if (scaffoldPipelineLayout_ == VK_NULL_HANDLE || scaffoldRenderPass_ == VK_NULL_HANDLE) {
      return;
    }

    const auto vertexShader = readBinaryFile("assets/shaders/scaffold.vert.spv");
    const auto fragmentShader = readBinaryFile("assets/shaders/scaffold.frag.spv");
    if (!vertexShader.has_value() || !fragmentShader.has_value()) {
      VOLT_LOG_ERROR_CAT(
          volt::core::logging::Category::kRender,
          "Missing UI shaders at assets/shaders/scaffold.vert.spv or scaffold.frag.spv");
      uiGraphicsPipeline_ = VK_NULL_HANDLE;
      return;
    }

    VkShaderModule vertShaderModule = createShaderModule(vertexShader.value());
    VkShaderModule fragShaderModule = createShaderModule(fragmentShader.value());

    VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
    vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertShaderStageInfo.module = vertShaderModule;
    vertShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
    fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragShaderStageInfo.module = fragShaderModule;
    fragShaderStageInfo.pName = "main";

    const std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages = {
        vertShaderStageInfo,
        fragShaderStageInfo,
    };

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkVertexInputBindingDescription bindingDescription{};
    bindingDescription.binding = 0;
    bindingDescription.stride = static_cast<uint32_t>(sizeof(volt::ui::UiVertex));
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 3> attributeDescriptions{};
    attributeDescriptions[0].location = 0;
    attributeDescriptions[0].binding = 0;
    attributeDescriptions[0].format = VK_FORMAT_R32G32_SFLOAT;
    attributeDescriptions[0].offset = static_cast<uint32_t>(offsetof(volt::ui::UiVertex, x));

    attributeDescriptions[1].location = 1;
    attributeDescriptions[1].binding = 0;
    attributeDescriptions[1].format = VK_FORMAT_R32G32_SFLOAT;
    attributeDescriptions[1].offset = static_cast<uint32_t>(offsetof(volt::ui::UiVertex, u));

    attributeDescriptions[2].location = 2;
    attributeDescriptions[2].binding = 0;
    attributeDescriptions[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attributeDescriptions[2].offset = static_cast<uint32_t>(offsetof(volt::ui::UiVertex, color));

    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0F;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
        VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_TRUE;
    colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    constexpr std::array<VkDynamicState, 2> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };

    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
    pipelineInfo.pStages = shaderStages.data();
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = scaffoldPipelineLayout_;
    pipelineInfo.renderPass = scaffoldRenderPass_;
    pipelineInfo.subpass = 0;

    const VkResult pipelineResult = vkCreateGraphicsPipelines(
        device_,
        VK_NULL_HANDLE,
        1,
        &pipelineInfo,
        nullptr,
        &uiGraphicsPipeline_);

    vkDestroyShaderModule(device_, fragShaderModule, nullptr);
    vkDestroyShaderModule(device_, vertShaderModule, nullptr);

    if (pipelineResult != VK_SUCCESS) {
      throw std::runtime_error("Failed to create UI graphics pipeline");
    }
  }

  void VulkanRenderer::createScaffoldFramebuffers()
  {
    if (scaffoldRenderPass_ == VK_NULL_HANDLE) {
      throw std::runtime_error("Cannot create scaffold framebuffers without render pass");
    }

    if (scaffoldDepthImageView_ == VK_NULL_HANDLE) {
      throw std::runtime_error("Cannot create scaffold framebuffers without depth image view");
    }

    scaffoldFramebuffers_.resize(swapchainImageViews_.size(), VK_NULL_HANDLE);

    for (std::size_t i = 0; i < swapchainImageViews_.size(); ++i) {
      VkImageView attachments[] = {swapchainImageViews_[i], scaffoldDepthImageView_};

      VkFramebufferCreateInfo framebufferInfo{};
      framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
      framebufferInfo.renderPass = scaffoldRenderPass_;
      framebufferInfo.attachmentCount = 2;
      framebufferInfo.pAttachments = attachments;
      framebufferInfo.width = swapchainExtent_.width;
      framebufferInfo.height = swapchainExtent_.height;
      framebufferInfo.layers = 1;

      if (vkCreateFramebuffer(device_, &framebufferInfo, nullptr, &scaffoldFramebuffers_[i]) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create scaffold framebuffer");
      }
    }
  }

  void VulkanRenderer::createScaffoldDepthResources()
  {
    scaffoldDepthFormat_ = findDepthFormat();

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = swapchainExtent_.width;
    imageInfo.extent.height = swapchainExtent_.height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = scaffoldDepthFormat_;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device_, &imageInfo, nullptr, &scaffoldDepthImage_) != VK_SUCCESS) {
      throw std::runtime_error("Failed to create scaffold depth image");
    }

    VkMemoryRequirements memoryRequirements{};
    vkGetImageMemoryRequirements(device_, scaffoldDepthImage_, &memoryRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memoryRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(
        memoryRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(device_, &allocInfo, nullptr, &scaffoldDepthImageMemory_) != VK_SUCCESS) {
      throw std::runtime_error("Failed to allocate scaffold depth image memory");
    }

    if (vkBindImageMemory(device_, scaffoldDepthImage_, scaffoldDepthImageMemory_, 0) != VK_SUCCESS) {
      throw std::runtime_error("Failed to bind scaffold depth image memory");
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = scaffoldDepthImage_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = scaffoldDepthFormat_;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device_, &viewInfo, nullptr, &scaffoldDepthImageView_) != VK_SUCCESS) {
      throw std::runtime_error("Failed to create scaffold depth image view");
    }
  }

  void VulkanRenderer::cleanupScaffoldDepthResources()
  {
    if (scaffoldDepthImageView_ != VK_NULL_HANDLE) {
      vkDestroyImageView(device_, scaffoldDepthImageView_, nullptr);
      scaffoldDepthImageView_ = VK_NULL_HANDLE;
    }

    if (scaffoldDepthImage_ != VK_NULL_HANDLE) {
      vkDestroyImage(device_, scaffoldDepthImage_, nullptr);
      scaffoldDepthImage_ = VK_NULL_HANDLE;
    }

    if (scaffoldDepthImageMemory_ != VK_NULL_HANDLE) {
      vkFreeMemory(device_, scaffoldDepthImageMemory_, nullptr);
      scaffoldDepthImageMemory_ = VK_NULL_HANDLE;
    }

    scaffoldDepthFormat_ = VK_FORMAT_UNDEFINED;
  }

  void VulkanRenderer::cleanupScaffoldFramebuffers()
  {
    for (VkFramebuffer framebuffer : scaffoldFramebuffers_) {
      if (framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device_, framebuffer, nullptr);
      }
    }
    scaffoldFramebuffers_.clear();
  }

  void VulkanRenderer::cleanupScaffoldRenderPass()
  {
    if (scaffoldRenderPass_ != VK_NULL_HANDLE) {
      vkDestroyRenderPass(device_, scaffoldRenderPass_, nullptr);
      scaffoldRenderPass_ = VK_NULL_HANDLE;
    }
  }

  void VulkanRenderer::cleanupScaffoldGraphicsPipeline()
  {
    if (scaffoldGraphicsPipeline_ != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, scaffoldGraphicsPipeline_, nullptr);
      scaffoldGraphicsPipeline_ = VK_NULL_HANDLE;
    }
  }

  void VulkanRenderer::cleanupUiGraphicsPipeline()
  {
    if (uiGraphicsPipeline_ != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, uiGraphicsPipeline_, nullptr);
      uiGraphicsPipeline_ = VK_NULL_HANDLE;
    }
  }

  void VulkanRenderer::cleanupScaffoldPipelineLayout()
  {
    if (scaffoldPipelineLayout_ != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(device_, scaffoldPipelineLayout_, nullptr);
      scaffoldPipelineLayout_ = VK_NULL_HANDLE;
    }

    if (uiDescriptorSetLayout_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorSetLayout(device_, uiDescriptorSetLayout_, nullptr);
      uiDescriptorSetLayout_ = VK_NULL_HANDLE;
    }
  }

  void VulkanRenderer::cleanupUiTextureResources()
  {
    uiDescriptorSet_ = VK_NULL_HANDLE;

    for (auto it = uiTextureResources_.begin(); it != uiTextureResources_.end();) {
      const std::string key = it->first;
      ++it;
      destroyUiTextureResource(key);
    }
    unresolvedTextureKeysLogged_.clear();

    if (uiDescriptorPool_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorPool(device_, uiDescriptorPool_, nullptr);
      uiDescriptorPool_ = VK_NULL_HANDLE;
    }

    if (uiTextureSampler_ != VK_NULL_HANDLE) {
      vkDestroySampler(device_, uiTextureSampler_, nullptr);
      uiTextureSampler_ = VK_NULL_HANDLE;
    }

    uiTextureImage_ = VK_NULL_HANDLE;
    uiTextureImageMemory_ = VK_NULL_HANDLE;
    uiTextureImageView_ = VK_NULL_HANDLE;
  }

  void VulkanRenderer::cleanupFrameScaffoldResources()
  {
    for (FrameSync& sync : frameSync_) {
      if (sync.imageAvailable != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_, sync.imageAvailable, nullptr);
        sync.imageAvailable = VK_NULL_HANDLE;
      }

      if (sync.renderFinished != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_, sync.renderFinished, nullptr);
        sync.renderFinished = VK_NULL_HANDLE;
      }

      if (sync.inFlight != VK_NULL_HANDLE) {
        vkDestroyFence(device_, sync.inFlight, nullptr);
        sync.inFlight = VK_NULL_HANDLE;
      }
    }

    destroyScaffoldCommandBuffers();

    if (scaffoldCommandPool_ != VK_NULL_HANDLE) {
      vkDestroyCommandPool(device_, scaffoldCommandPool_, nullptr);
      scaffoldCommandPool_ = VK_NULL_HANDLE;
    }
  }

  void VulkanRenderer::createScaffoldCommandPool()
  {
    const QueueFamilyIndices indices = findQueueFamilies(physicalDevice_);

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = indices.graphicsFamily.value();

    if (vkCreateCommandPool(device_, &poolInfo, nullptr, &scaffoldCommandPool_) != VK_SUCCESS) {
      throw std::runtime_error("Failed to create scaffold command pool");
    }
  }

  void VulkanRenderer::allocateScaffoldCommandBuffers()
  {
    if (scaffoldCommandPool_ == VK_NULL_HANDLE || swapchainImages_.empty()) {
      return;
    }

    scaffoldCommandBuffers_.resize(swapchainImages_.size());

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = scaffoldCommandPool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = static_cast<uint32_t>(scaffoldCommandBuffers_.size());

    if (vkAllocateCommandBuffers(device_, &allocInfo, scaffoldCommandBuffers_.data()) != VK_SUCCESS) {
      throw std::runtime_error("Failed to allocate scaffold command buffers");
    }
  }

  void VulkanRenderer::recreateScaffoldCommandBuffers()
  {
    destroyScaffoldCommandBuffers();
    allocateScaffoldCommandBuffers();
  }

  void VulkanRenderer::createScaffoldSyncObjects()
  {
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (FrameSync& sync : frameSync_) {
      if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &sync.imageAvailable) != VK_SUCCESS ||
          vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &sync.renderFinished) != VK_SUCCESS ||
          vkCreateFence(device_, &fenceInfo, nullptr, &sync.inFlight) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create frame scaffold synchronization objects");
      }
    }
  }

  void VulkanRenderer::destroyScaffoldCommandBuffers()
  {
    if (scaffoldCommandPool_ == VK_NULL_HANDLE || scaffoldCommandBuffers_.empty()) {
      return;
    }

    vkFreeCommandBuffers(
        device_,
        scaffoldCommandPool_,
        static_cast<uint32_t>(scaffoldCommandBuffers_.size()),
        scaffoldCommandBuffers_.data());
    scaffoldCommandBuffers_.clear();
  }

  void VulkanRenderer::ensureUiScaffoldBufferCapacity(std::size_t vertexBytes, std::size_t indexBytes)
  {
    const bool vertexEnough = uiVertexBuffer_ != VK_NULL_HANDLE && uiVertexBufferCapacityBytes_ >= vertexBytes;
    const bool indexEnough = uiIndexBuffer_ != VK_NULL_HANDLE && uiIndexBufferCapacityBytes_ >= indexBytes;
    if (vertexEnough && indexEnough) {
      return;
    }

    destroyUiScaffoldBuffers();
    createUiScaffoldBuffers(vertexBytes, indexBytes);
  }

  void VulkanRenderer::createUiScaffoldBuffers(std::size_t vertexBytes, std::size_t indexBytes)
  {
    if (vertexBytes == 0 || indexBytes == 0) {
      return;
    }

    VkBufferCreateInfo vertexBufferInfo{};
    vertexBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    vertexBufferInfo.size = static_cast<VkDeviceSize>(vertexBytes);
    vertexBufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    vertexBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device_, &vertexBufferInfo, nullptr, &uiVertexBuffer_) != VK_SUCCESS) {
      throw std::runtime_error("Failed to create UI scaffold vertex buffer");
    }

    VkMemoryRequirements vertexMemoryRequirements{};
    vkGetBufferMemoryRequirements(device_, uiVertexBuffer_, &vertexMemoryRequirements);

    VkMemoryAllocateInfo vertexAllocInfo{};
    vertexAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    vertexAllocInfo.allocationSize = vertexMemoryRequirements.size;
    vertexAllocInfo.memoryTypeIndex = findMemoryType(
        vertexMemoryRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(device_, &vertexAllocInfo, nullptr, &uiVertexBufferMemory_) != VK_SUCCESS) {
      throw std::runtime_error("Failed to allocate UI scaffold vertex memory");
    }

    if (vkBindBufferMemory(device_, uiVertexBuffer_, uiVertexBufferMemory_, 0) != VK_SUCCESS) {
      throw std::runtime_error("Failed to bind UI scaffold vertex memory");
    }

    VkBufferCreateInfo indexBufferInfo{};
    indexBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    indexBufferInfo.size = static_cast<VkDeviceSize>(indexBytes);
    indexBufferInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    indexBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device_, &indexBufferInfo, nullptr, &uiIndexBuffer_) != VK_SUCCESS) {
      throw std::runtime_error("Failed to create UI scaffold index buffer");
    }

    VkMemoryRequirements indexMemoryRequirements{};
    vkGetBufferMemoryRequirements(device_, uiIndexBuffer_, &indexMemoryRequirements);

    VkMemoryAllocateInfo indexAllocInfo{};
    indexAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    indexAllocInfo.allocationSize = indexMemoryRequirements.size;
    indexAllocInfo.memoryTypeIndex = findMemoryType(
        indexMemoryRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(device_, &indexAllocInfo, nullptr, &uiIndexBufferMemory_) != VK_SUCCESS) {
      throw std::runtime_error("Failed to allocate UI scaffold index memory");
    }

    if (vkBindBufferMemory(device_, uiIndexBuffer_, uiIndexBufferMemory_, 0) != VK_SUCCESS) {
      throw std::runtime_error("Failed to bind UI scaffold index memory");
    }

    uiVertexBufferCapacityBytes_ = vertexBytes;
    uiIndexBufferCapacityBytes_ = indexBytes;
  }

  void VulkanRenderer::destroyUiScaffoldBuffers()
  {
    if (uiVertexBuffer_ != VK_NULL_HANDLE) {
      vkDestroyBuffer(device_, uiVertexBuffer_, nullptr);
      uiVertexBuffer_ = VK_NULL_HANDLE;
    }

    if (uiVertexBufferMemory_ != VK_NULL_HANDLE) {
      vkFreeMemory(device_, uiVertexBufferMemory_, nullptr);
      uiVertexBufferMemory_ = VK_NULL_HANDLE;
    }

    if (uiIndexBuffer_ != VK_NULL_HANDLE) {
      vkDestroyBuffer(device_, uiIndexBuffer_, nullptr);
      uiIndexBuffer_ = VK_NULL_HANDLE;
    }

    if (uiIndexBufferMemory_ != VK_NULL_HANDLE) {
      vkFreeMemory(device_, uiIndexBufferMemory_, nullptr);
      uiIndexBufferMemory_ = VK_NULL_HANDLE;
    }

    uiVertexBufferCapacityBytes_ = 0;
    uiIndexBufferCapacityBytes_ = 0;
  }

  void VulkanRenderer::uploadUiMeshData(const volt::ui::UiMeshData& meshData)
  {
    if (meshData.vertices.empty() || meshData.indices.empty()) {
      return;
    }

    const std::size_t vertexBytes = meshData.vertices.size() * sizeof(volt::ui::UiVertex);
    const std::size_t indexBytes = meshData.indices.size() * sizeof(std::uint32_t);
    ensureUiScaffoldBufferCapacity(vertexBytes, indexBytes);

    void* vertexMapped = nullptr;
    if (vkMapMemory(device_, uiVertexBufferMemory_, 0, static_cast<VkDeviceSize>(vertexBytes), 0, &vertexMapped) != VK_SUCCESS) {
      throw std::runtime_error("Failed to map UI scaffold vertex memory");
    }

    std::vector<volt::ui::UiVertex> clipVertices = meshData.vertices;
    const float width = static_cast<float>(swapchainExtent_.width);
    const float height = static_cast<float>(swapchainExtent_.height);
    if (width > 0.0F && height > 0.0F) {
      for (volt::ui::UiVertex& vertex : clipVertices) {
        vertex.x = ((vertex.x / width) * 2.0F) - 1.0F;
        vertex.y = ((vertex.y / height) * 2.0F) - 1.0F;
      }
    }

    std::memcpy(vertexMapped, clipVertices.data(), vertexBytes);
    vkUnmapMemory(device_, uiVertexBufferMemory_);

    void* indexMapped = nullptr;
    if (vkMapMemory(device_, uiIndexBufferMemory_, 0, static_cast<VkDeviceSize>(indexBytes), 0, &indexMapped) != VK_SUCCESS) {
      throw std::runtime_error("Failed to map UI scaffold index memory");
    }
    std::memcpy(indexMapped, meshData.indices.data(), indexBytes);
    vkUnmapMemory(device_, uiIndexBufferMemory_);
  }

    void VulkanRenderer::recordUiMeshDraws(
      VkCommandBuffer commandBuffer,
      const volt::ui::UiMeshData& meshData)
  {
    if (uiVertexBuffer_ == VK_NULL_HANDLE || uiIndexBuffer_ == VK_NULL_HANDLE ||
        uiGraphicsPipeline_ == VK_NULL_HANDLE || uiDescriptorSet_ == VK_NULL_HANDLE) {
      return;
    }

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, uiGraphicsPipeline_);

    VkDeviceSize vertexOffset = 0;
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &uiVertexBuffer_, &vertexOffset);
    vkCmdBindIndexBuffer(commandBuffer, uiIndexBuffer_, 0, VK_INDEX_TYPE_UINT32);

    VkDescriptorSet lastBoundDescriptor = VK_NULL_HANDLE;
    for (const volt::ui::UiMeshBatch& batch : meshData.batches) {
      const VkDescriptorSet descriptorSet = resolveUiDescriptorSetForBatch(batch.textureKey);
      if (descriptorSet != VK_NULL_HANDLE && descriptorSet != lastBoundDescriptor) {
        vkCmdBindDescriptorSets(
            commandBuffer,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            scaffoldPipelineLayout_,
            0,
            1,
            &descriptorSet,
            0,
            nullptr);
        lastBoundDescriptor = descriptorSet;
      }

      VkRect2D scissor{};
      scissor.offset.x = static_cast<std::int32_t>(std::max(0.0F, batch.clipRect.x));
      scissor.offset.y = static_cast<std::int32_t>(std::max(0.0F, batch.clipRect.y));
      scissor.extent.width = static_cast<std::uint32_t>(std::max(0.0F, batch.clipRect.width));
      scissor.extent.height = static_cast<std::uint32_t>(std::max(0.0F, batch.clipRect.height));

      if (scissor.extent.width == 0U || scissor.extent.height == 0U) {
        continue;
      }

      vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
      vkCmdDrawIndexed(commandBuffer, batch.indexCount, 1, batch.firstIndex, 0, 0);
    }
  }

  VkDescriptorSet VulkanRenderer::resolveUiDescriptorSetForBatch(const std::string& textureKey)
  {
    const std::string key = textureKey.empty() ? "__white" : textureKey;

    if (key != "__white" && volt::io::AssetManager::instance().hasImageChanged(key)) {
      destroyUiTextureResource(key);
      unresolvedTextureKeysLogged_.erase(key);
    }

    auto it = uiTextureResources_.find(key);
    if (it == uiTextureResources_.end()) {
      createUiTextureResourceForKey(key);
      it = uiTextureResources_.find(key);
    }

    if (it != uiTextureResources_.end()) {
      return it->second.descriptorSet;
    }

    if (unresolvedTextureKeysLogged_.insert(key).second) {
      VOLT_LOG_WARN_CAT(
          volt::core::logging::Category::kRender,
          "Failed to resolve UI texture key, using white fallback: ",
          key);
    }

    return uiDescriptorSet_;
  }

  void VulkanRenderer::createInstance(const char *appName)
  {
    uint32_t extensionCount = 0;
    const char **glfwExtensions = glfwGetRequiredInstanceExtensions(&extensionCount);
    if (glfwExtensions == nullptr || extensionCount == 0)
    {
      throw std::runtime_error("GLFW did not provide Vulkan instance extensions");
    }

    std::vector<const char *> requiredExtensions(glfwExtensions, glfwExtensions + extensionCount);

#if defined(VOLT_ENABLE_DEBUG_LOGGING) && VOLT_ENABLE_DEBUG_LOGGING
    const bool debugUtilsAvailable = hasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (debugUtilsAvailable) {
      requiredExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    } else {
      VOLT_LOG_WARN_CAT(
          volt::core::logging::Category::kRender,
          "Vulkan debug utils extension unavailable; validation reports disabled");
    }
#endif

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

#if defined(VOLT_ENABLE_DEBUG_LOGGING) && VOLT_ENABLE_DEBUG_LOGGING
    const bool validationLayerAvailable = hasValidationLayer(kValidationLayerName);
    if (validationLayerAvailable) {
      createInfo.enabledLayerCount = 1;
      createInfo.ppEnabledLayerNames = &kValidationLayerName;
    } else {
      VOLT_LOG_WARN_CAT(
          volt::core::logging::Category::kRender,
          "Validation layer unavailable: ",
          kValidationLayerName);
    }
#endif

    const VkResult result = vkCreateInstance(&createInfo, nullptr, &instance_);
    if (result != VK_SUCCESS)
    {
      throw std::runtime_error("Failed to create Vulkan instance");
    }
  }

  void VulkanRenderer::setupDebugMessenger()
  {
#if defined(VOLT_ENABLE_DEBUG_LOGGING) && VOLT_ENABLE_DEBUG_LOGGING
    if (instance_ == VK_NULL_HANDLE || !hasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
      return;
    }

    VkDebugUtilsMessengerCreateInfoEXT createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createInfo.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createInfo.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = debugUtilsCallback;

    auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
    if (createMessenger == nullptr) {
      VOLT_LOG_WARN_CAT(
          volt::core::logging::Category::kRender,
          "vkCreateDebugUtilsMessengerEXT unavailable");
      return;
    }

    if (createMessenger(instance_, &createInfo, nullptr, &debugMessenger_) == VK_SUCCESS) {
      VOLT_LOG_INFO_CAT(volt::core::logging::Category::kRender, "Vulkan debug messenger enabled");
    }
#endif
  }

  void VulkanRenderer::cleanupDebugMessenger()
  {
    if (instance_ == VK_NULL_HANDLE || debugMessenger_ == VK_NULL_HANDLE) {
      return;
    }

    auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
    if (destroyMessenger != nullptr) {
      destroyMessenger(instance_, debugMessenger_, nullptr);
    }
    debugMessenger_ = VK_NULL_HANDLE;
  }

  void VulkanRenderer::createSurface()
  {
    const VkResult result = glfwCreateWindowSurface(instance_, window_, nullptr, &surface_);
    if (result != VK_SUCCESS)
    {
      throw std::runtime_error("Failed to create Vulkan surface");
    }
  }

  void VulkanRenderer::pickPhysicalDevice()
  {
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr);
    if (deviceCount == 0)
    {
      throw std::runtime_error("No Vulkan physical device found");
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data());

    for (VkPhysicalDevice device : devices)
    {
      if (isDeviceSuitable(device))
      {
        physicalDevice_ = device;
        break;
      }
    }

    if (physicalDevice_ == VK_NULL_HANDLE)
    {
      throw std::runtime_error("No suitable Vulkan physical device found");
    }
  }

  void VulkanRenderer::createLogicalDevice()
  {
    const QueueFamilyIndices indices = findQueueFamilies(physicalDevice_);

    std::set<uint32_t> uniqueQueueFamilies = {
        indices.graphicsFamily.value(),
        indices.presentFamily.value(),
    };

    constexpr float queuePriority = 1.0F;
    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    queueCreateInfos.reserve(uniqueQueueFamilies.size());

    for (uint32_t queueFamily : uniqueQueueFamilies)
    {
      VkDeviceQueueCreateInfo queueCreateInfo{};
      queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
      queueCreateInfo.queueFamilyIndex = queueFamily;
      queueCreateInfo.queueCount = 1;
      queueCreateInfo.pQueuePriorities = &queuePriority;
      queueCreateInfos.push_back(queueCreateInfo);
    }

    const std::vector<const char *> requiredDeviceExtensions = {
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
    if (result != VK_SUCCESS)
    {
      throw std::runtime_error("Failed to create Vulkan logical device");
    }

    vkGetDeviceQueue(device_, indices.graphicsFamily.value(), 0, &graphicsQueue_);
    vkGetDeviceQueue(device_, indices.presentFamily.value(), 0, &presentQueue_);
  }

  void VulkanRenderer::createSwapchain()
  {
    const SwapchainSupportDetails support = querySwapchainSupport(physicalDevice_);

    const VkSurfaceFormatKHR surfaceFormat = chooseSwapSurfaceFormat(support.formats);
    const VkPresentModeKHR presentMode = chooseSwapPresentMode(support.presentModes);
    const VkExtent2D extent = chooseSwapExtent(support.capabilities);

    uint32_t imageCount = support.capabilities.minImageCount + 1;
    if (support.capabilities.maxImageCount > 0 && imageCount > support.capabilities.maxImageCount)
    {
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

    if (indices.graphicsFamily != indices.presentFamily)
    {
      createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
      createInfo.queueFamilyIndexCount = 2;
      createInfo.pQueueFamilyIndices = queueFamilyIndices;
    }
    else
    {
      createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform = support.capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = VK_NULL_HANDLE;

    const VkResult result = vkCreateSwapchainKHR(device_, &createInfo, nullptr, &swapchain_);
    if (result != VK_SUCCESS)
    {
      throw std::runtime_error("Failed to create Vulkan swapchain");
    }

    vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr);
    swapchainImages_.resize(imageCount);
    vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, swapchainImages_.data());

    swapchainImageFormat_ = surfaceFormat.format;
    swapchainExtent_ = extent;
  }

  void VulkanRenderer::createSwapchainImageViews()
  {
    swapchainImageViews_.resize(swapchainImages_.size());

    for (size_t i = 0; i < swapchainImages_.size(); ++i)
    {
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

      if (vkCreateImageView(device_, &createInfo, nullptr, &swapchainImageViews_[i]) != VK_SUCCESS)
      {
        throw std::runtime_error("Failed to create Vulkan swapchain image view");
      }
    }
  }

  void VulkanRenderer::recreateSwapchain()
  {
    VOLT_LOG_DEBUG_CAT(volt::core::logging::Category::kRender, "Renderer swapchain recreation requested");

    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window_, &width, &height);

    while (width == 0 || height == 0)
    {
      glfwWaitEvents();
      glfwGetFramebufferSize(window_, &width, &height);
    }

    vkDeviceWaitIdle(device_);
    cleanupSwapchain();
    cleanupUiGraphicsPipeline();
    cleanupScaffoldGraphicsPipeline();
    cleanupScaffoldRenderPass();
    createSwapchain();
    createSwapchainImageViews();
    createScaffoldDepthResources();
    createScaffoldRenderPass();
    createScaffoldGraphicsPipeline();
    createUiGraphicsPipeline();
    createScaffoldFramebuffers();
    recreateScaffoldCommandBuffers();

    VOLT_LOG_DEBUG_CAT(volt::core::logging::Category::kRender, "Renderer swapchain recreation complete");
  }

  void VulkanRenderer::cleanupSwapchain()
  {
    cleanupScaffoldFramebuffers();
    cleanupScaffoldDepthResources();

    for (VkImageView imageView : swapchainImageViews_)
    {
      vkDestroyImageView(device_, imageView, nullptr);
    }
    swapchainImageViews_.clear();

    if (swapchain_ != VK_NULL_HANDLE)
    {
      vkDestroySwapchainKHR(device_, swapchain_, nullptr);
      swapchain_ = VK_NULL_HANDLE;
    }
    swapchainImages_.clear();
  }

  VulkanRenderer::QueueFamilyIndices VulkanRenderer::findQueueFamilies(VkPhysicalDevice device) const
  {
    QueueFamilyIndices indices;

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

    for (uint32_t i = 0; i < queueFamilyCount; ++i)
    {
      if ((queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
      {
        indices.graphicsFamily = i;
      }

      VkBool32 presentSupport = VK_FALSE;
      vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &presentSupport);
      if (presentSupport == VK_TRUE)
      {
        indices.presentFamily = i;
      }

      if (indices.isComplete())
      {
        break;
      }
    }

    return indices;
  }

  VulkanRenderer::SwapchainSupportDetails VulkanRenderer::querySwapchainSupport(VkPhysicalDevice device) const
  {
    SwapchainSupportDetails details;

    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface_, &details.capabilities);

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &formatCount, nullptr);
    if (formatCount > 0)
    {
      details.formats.resize(formatCount);
      vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &formatCount, details.formats.data());
    }

    uint32_t presentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &presentModeCount, nullptr);
    if (presentModeCount > 0)
    {
      details.presentModes.resize(presentModeCount);
      vkGetPhysicalDeviceSurfacePresentModesKHR(
          device,
          surface_,
          &presentModeCount,
          details.presentModes.data());
    }

    return details;
  }

  bool VulkanRenderer::isDeviceSuitable(VkPhysicalDevice device) const
  {
    const QueueFamilyIndices indices = findQueueFamilies(device);
    if (!indices.isComplete())
    {
      return false;
    }

    const SwapchainSupportDetails support = querySwapchainSupport(device);
    if (support.formats.empty() || support.presentModes.empty())
    {
      return false;
    }

    return true;
  }

  VkSurfaceFormatKHR VulkanRenderer::chooseSwapSurfaceFormat(
      const std::vector<VkSurfaceFormatKHR> &availableFormats) const
  {
    for (const VkSurfaceFormatKHR &availableFormat : availableFormats)
    {
      if (availableFormat.format == VK_FORMAT_B8G8R8A8_SRGB &&
          availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
      {
        return availableFormat;
      }
    }

    return availableFormats.front();
  }

  VkPresentModeKHR VulkanRenderer::chooseSwapPresentMode(
      const std::vector<VkPresentModeKHR> &availablePresentModes) const
  {
    for (VkPresentModeKHR availablePresentMode : availablePresentModes)
    {
      if (availablePresentMode == VK_PRESENT_MODE_MAILBOX_KHR)
      {
        return availablePresentMode;
      }
    }

    return VK_PRESENT_MODE_FIFO_KHR;
  }

  VkExtent2D VulkanRenderer::chooseSwapExtent(const VkSurfaceCapabilitiesKHR &capabilities) const
  {
    if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max())
    {
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

  VkFormat VulkanRenderer::findSupportedFormat(
      const std::vector<VkFormat>& candidates,
      VkImageTiling tiling,
      VkFormatFeatureFlags features) const
  {
    for (VkFormat format : candidates) {
      VkFormatProperties properties{};
      vkGetPhysicalDeviceFormatProperties(physicalDevice_, format, &properties);

      if (tiling == VK_IMAGE_TILING_LINEAR &&
          (properties.linearTilingFeatures & features) == features) {
        return format;
      }

      if (tiling == VK_IMAGE_TILING_OPTIMAL &&
          (properties.optimalTilingFeatures & features) == features) {
        return format;
      }
    }

    throw std::runtime_error("Failed to find supported Vulkan format for scaffold resource");
  }

  VkFormat VulkanRenderer::findDepthFormat() const
  {
    return findSupportedFormat(
        {
            VK_FORMAT_D32_SFLOAT,
            VK_FORMAT_D32_SFLOAT_S8_UINT,
            VK_FORMAT_D24_UNORM_S8_UINT,
        },
        VK_IMAGE_TILING_OPTIMAL,
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
  }

  std::uint32_t VulkanRenderer::findMemoryType(
      std::uint32_t typeFilter,
      VkMemoryPropertyFlags properties) const
  {
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memoryProperties);

    for (std::uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
      if ((typeFilter & (1U << i)) != 0U &&
          (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) {
        return i;
      }
    }

    throw std::runtime_error("Failed to find suitable Vulkan memory type for scaffold resource");
  }

  VkShaderModule VulkanRenderer::createShaderModule(const std::vector<char>& code) const
  {
    if (code.empty() || (code.size() % 4U) != 0U) {
      throw std::runtime_error("Invalid SPIR-V shader bytecode for scaffold pipeline");
    }

    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const std::uint32_t*>(code.data());

    VkShaderModule shaderModule = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
      throw std::runtime_error("Failed to create scaffold shader module");
    }

    return shaderModule;
  }

} // namespace volt::render
