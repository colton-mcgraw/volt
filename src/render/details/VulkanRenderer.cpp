#include "volt/render/details/VulkanRenderer.hpp"

#include "volt/core/Logging.hpp"
#include "volt/event/Event.hpp"
#include "volt/event/EventDispatcher.hpp"
#include "volt/io/assets/AssetManager.hpp"
#include "volt/io/assets/Font.hpp"
#include "volt/io/assets/Manifest.hpp"
#include "volt/math/Math.hpp"
#include "volt/math/Projection.hpp"
#include "volt/platform/Window.hpp"
#include "volt/ui/UIMesh.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <vulkan/vulkan_win32.h>
#endif

namespace volt::render::details
{

  namespace
  {
    constexpr std::uint32_t kRendererVersionMajor = 0;
    constexpr std::uint32_t kRendererVersionMinor = 1;
    constexpr std::uint32_t kRendererVersionPatch = 0;
    constexpr double kUiSlowPathWarnThresholdMs = 10.0;
    constexpr double kFrameStallWarnThresholdMs = 50.0;
    constexpr auto kFrameStallLogCooldown = std::chrono::milliseconds(500);

    bool equalsAsciiCaseInsensitive(std::string_view lhs, std::string_view rhs)
    {
      if (lhs.size() != rhs.size()) {
        return false;
      }

      for (std::size_t i = 0; i < lhs.size(); ++i) {
        const unsigned char left = static_cast<unsigned char>(lhs[i]);
        const unsigned char right = static_cast<unsigned char>(rhs[i]);
        if (std::tolower(left) != std::tolower(right)) {
          return false;
        }
      }

      return true;
    }

    const char* presentModeName(VkPresentModeKHR presentMode)
    {
      switch (presentMode) {
        case VK_PRESENT_MODE_IMMEDIATE_KHR:
          return "immediate";
        case VK_PRESENT_MODE_MAILBOX_KHR:
          return "mailbox";
        case VK_PRESENT_MODE_FIFO_KHR:
          return "fifo";
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
          return "fifo_relaxed";
        default:
          return "unknown";
      }
    }

    std::optional<VkPresentModeKHR> parsePresentModeName(std::string_view value)
    {
      if (equalsAsciiCaseInsensitive(value, "immediate")) {
        return VK_PRESENT_MODE_IMMEDIATE_KHR;
      }
      if (equalsAsciiCaseInsensitive(value, "mailbox")) {
        return VK_PRESENT_MODE_MAILBOX_KHR;
      }
      if (equalsAsciiCaseInsensitive(value, "fifo")) {
        return VK_PRESENT_MODE_FIFO_KHR;
      }
      if (equalsAsciiCaseInsensitive(value, "fifo_relaxed")) {
        return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
      }
      if (equalsAsciiCaseInsensitive(value, "auto") || value.empty()) {
        return std::nullopt;
      }
      return std::nullopt;
    }

    std::optional<std::string> tryGetEnvString(const char* name)
    {
#if defined(_WIN32)
      char* value = nullptr;
      std::size_t valueLength = 0U;
      if (_dupenv_s(&value, &valueLength, name) != 0 || value == nullptr || valueLength == 0U) {
        if (value != nullptr) {
          std::free(value);
        }
        return std::nullopt;
      }

      std::string result{value};
      std::free(value);
      if (result.empty()) {
        return std::nullopt;
      }
      return result;
#else
      const char* value = std::getenv(name);
      if (value == nullptr || value[0] == '\0') {
        return std::nullopt;
      }
      return std::string{value};
#endif
    }

    std::string configuredPresentModeName()
    {
      if (const auto envValue = tryGetEnvString("VOLT_PRESENT_MODE"); envValue.has_value()) {
        return *envValue;
      }

      volt::io::KeyValueManifest& manifest = volt::io::manifestService();
      manifest.refresh(false);
      if (const auto configuredValue = manifest.findString("present-mode"); configuredValue.has_value()) {
        return *configuredValue;
      }

      return {};
    }

    bool presentModeAvailable(
        const std::vector<VkPresentModeKHR>& availablePresentModes,
        VkPresentModeKHR requestedPresentMode)
    {
      return std::find(
                 availablePresentModes.begin(),
                 availablePresentModes.end(),
                 requestedPresentMode) != availablePresentModes.end();
    }

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

    constexpr std::uint32_t kVectorTextTileSize = 16U;

    std::uint32_t computeExclusiveScanTotal(const std::vector<std::uint32_t>& counts, std::vector<std::uint32_t>& outOffsets)
    {
      outOffsets.resize(counts.size());
      std::uint32_t total = 0U;
      for (std::size_t i = 0U; i < counts.size(); ++i) {
        outOffsets[i] = total;
        total += counts[i];
      }
      return total;
    }

  } // namespace

  bool VulkanRenderer::QueueFamilyIndices::isComplete() const
  {
    return graphicsFamily.has_value() && presentFamily.has_value();
  }

  VulkanRenderer::VulkanRenderer(volt::platform::Window& window, const char *appName) : window_(&window)
  {
    if (window_ == nullptr)
    {
      throw std::invalid_argument("VulkanRenderer requires a valid window handle");
    }

    // Initialize vector text worker thread for async data prep
    vectorTextWorker_ = std::make_unique<VectorTextWorker>();

    createInstance(appName);
    setupDebugMessenger();
    createSurface();
    pickPhysicalDevice();
    createLogicalDevice();
    createSwapchain();
    createSwapchainImageViews();
    createScaffoldDepthResources();
    createScaffoldRenderPass();
    createUiOffscreenRenderPass();
    createScaffoldPipelineLayout();
    createScaffoldGraphicsPipeline();
    createUiGraphicsPipeline();
    createUiOpaqueGraphicsPipeline();
    createUiTextSdfGraphicsPipeline();
    createUiOffscreenGraphicsPipeline();
    createUiOffscreenTextSdfGraphicsPipeline();
    createScaffoldFramebuffers();
    createFrameScaffoldResources();
    createUiTextureResources();
    VOLT_LOG_INFO_CAT(volt::core::logging::Category::kRender, "Renderer initialized for app: ", appName);
  }

  VulkanRenderer::~VulkanRenderer()
  {
    // Shutdown vector text worker thread
    if (vectorTextWorker_) {
      vectorTextWorker_->shutdown();
      vectorTextWorker_.reset();
    }

    if (device_ != VK_NULL_HANDLE)
    {
      vkDeviceWaitIdle(device_);
    }

    cleanupFrameScaffoldResources();
    destroyUiScaffoldBuffers();
    cleanupUiTextureResources();
    cleanupSwapchain();
    cleanupUiTextSdfGraphicsPipeline();
    cleanupUiOpaqueGraphicsPipeline();
    cleanupUiGraphicsPipeline();
    cleanupUiOffscreenTextSdfGraphicsPipeline();
    cleanupUiOffscreenGraphicsPipeline();
    cleanupScaffoldGraphicsPipeline();
    cleanupScaffoldPipelineLayout();
    cleanupUiOffscreenRenderPass();
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

    const auto [framebufferWidth, framebufferHeight] = window_->framebufferExtent();
    if (framebufferWidth == 0U || framebufferHeight == 0U)
    {
      return;
    }

    if (framebufferWidth != swapchainExtent_.width ||
        framebufferHeight != swapchainExtent_.height)
    {
      if (resizeDebounceElapsed)
      {
        recreateSwapchain();
        const auto [resizeWidth, resizeHeight] = window_->framebufferExtent();
        if (resizeWidth == 0U || resizeHeight == 0U)
        {
          return;
        }
      }
    }

    updateScaffoldCameraMatrices();

    latestFrameCpuTimings_ = {};

    const auto beginStart = std::chrono::steady_clock::now();
    if (!beginFrameScaffold())
    {
      return;
    }
    const auto beginEnd = std::chrono::steady_clock::now();

    const auto recordStart = beginEnd;
    recordScenePassScaffold();
    // Split UI record into upload and draw timings
    auto uiUploadStart = std::chrono::steady_clock::now();
    bool didUiUpload = false;
    if (uiMeshProvider_) {
      const volt::ui::UiMeshData* meshData = uiMeshProvider_();
      if (meshData != nullptr && !meshData->vertices.empty() && !meshData->indices.empty()) {
        uploadUiMeshData(*meshData);
        didUiUpload = true;
      }
    }
    auto uiUploadEnd = std::chrono::steady_clock::now();
    auto uiDrawStart = uiUploadEnd;
    if (didUiUpload && acquiredImageIndex_ < scaffoldCommandBuffers_.size()) {
      const volt::ui::UiMeshData* meshData = uiMeshProvider_();
      if (meshData != nullptr && !meshData->vertices.empty() && !meshData->indices.empty()) {
        recordUiMeshDraws(scaffoldCommandBuffers_[acquiredImageIndex_], *meshData);
      }
    }
    auto uiDrawEnd = std::chrono::steady_clock::now();
    const auto recordEnd = uiDrawEnd;

    const auto endStart = recordEnd;
    endFrameScaffold();
    const auto endEnd = std::chrono::steady_clock::now();

    latestFrameCpuTimings_.recordMs = static_cast<std::int32_t>(std::lround(
      std::chrono::duration<double, std::milli>(recordEnd - recordStart).count()));
    latestFrameCpuTimings_.recordUploadMs = static_cast<std::int32_t>(std::lround(
      std::chrono::duration<double, std::milli>(uiUploadEnd - uiUploadStart).count()));
    latestFrameCpuTimings_.recordDrawMs = static_cast<std::int32_t>(std::lround(
      std::chrono::duration<double, std::milli>(uiDrawEnd - uiDrawStart).count()));

    const std::int32_t beginMs = static_cast<std::int32_t>(std::lround(
        std::chrono::duration<double, std::milli>(beginEnd - beginStart).count()));
    const std::int32_t endMs = static_cast<std::int32_t>(std::lround(
        std::chrono::duration<double, std::milli>(endEnd - endStart).count()));
    const std::int32_t knownBeginMs = latestFrameCpuTimings_.fenceWaitMs + latestFrameCpuTimings_.acquireMs;
    const std::int32_t knownEndMs = latestFrameCpuTimings_.submitMs + latestFrameCpuTimings_.presentMs;
    if (beginMs > knownBeginMs) {
      latestFrameCpuTimings_.acquireMs += beginMs - knownBeginMs;
    }
    if (endMs > knownEndMs) {
      latestFrameCpuTimings_.presentMs += endMs - knownEndMs;
    }
    latestFrameCpuTimings_.valid = true;

    const double frameStallMs = std::max({
        static_cast<double>(latestFrameCpuTimings_.fenceWaitMs),
        static_cast<double>(latestFrameCpuTimings_.acquireMs),
        static_cast<double>(latestFrameCpuTimings_.recordUploadMs),
        static_cast<double>(latestFrameCpuTimings_.recordDrawMs),
        static_cast<double>(latestFrameCpuTimings_.uiVectorTextPrepMs),
        static_cast<double>(latestFrameCpuTimings_.submitMs),
        static_cast<double>(latestFrameCpuTimings_.presentMs),
        latestFrameCpuTimings_.uiDescriptorResolveTotalMs,
        latestFrameCpuTimings_.uiDrawCallMaxMs,
    });
    const auto now = std::chrono::steady_clock::now();
    if (frameStallMs >= kFrameStallWarnThresholdMs &&
        (lastFrameStallLogAt_ == std::chrono::steady_clock::time_point{} ||
         (now - lastFrameStallLogAt_) >= kFrameStallLogCooldown)) {
      VOLT_LOG_WARN_CAT(
          volt::core::logging::Category::kRender,
          "Frame stall | fenceWait=", latestFrameCpuTimings_.fenceWaitMs,
          " ms | acquire=", latestFrameCpuTimings_.acquireMs,
          " ms | upload=", latestFrameCpuTimings_.recordUploadMs,
          " ms | draw=", latestFrameCpuTimings_.recordDrawMs,
          " ms | vectorTextPrep=", latestFrameCpuTimings_.uiVectorTextPrepMs,
          " ms | submit=", latestFrameCpuTimings_.submitMs,
          " ms | present=", latestFrameCpuTimings_.presentMs,
          " ms | texResolveTotal=", latestFrameCpuTimings_.uiDescriptorResolveTotalMs,
          " ms | slowestDraw=", latestFrameCpuTimings_.uiDrawCallMaxMs,
          " ms @batch=", latestFrameCpuTimings_.uiDrawCallMaxBatchIndex,
          " tex=", latestFrameCpuTimings_.uiDrawCallMaxTextureKey,
          " | slowestResolveTex=", latestFrameCpuTimings_.uiDescriptorResolveMaxTextureKey,
          " (", latestFrameCpuTimings_.uiDescriptorResolveMaxMs, " ms)");
      lastFrameStallLogAt_ = now;
    }
  }

  Renderer::VectorTextGpuTimings VulkanRenderer::vectorTextGpuTimings() const
  {
    return latestVectorTextGpuTimings_;
  }

  Renderer::FrameCpuTimings VulkanRenderer::frameCpuTimings() const
  {
    return latestFrameCpuTimings_;
  }

  bool VulkanRenderer::beginFrameScaffold()
  {
    FrameSync& sync = frameSync_[currentFrameSlot_];
    if (sync.inFlight != VK_NULL_HANDLE) {
      const auto fenceWaitStart = std::chrono::steady_clock::now();
      const VkResult waitResult = vkWaitForFences(device_, 1, &sync.inFlight, VK_TRUE, UINT64_MAX);
      const auto fenceWaitEnd = std::chrono::steady_clock::now();
      latestFrameCpuTimings_.fenceWaitMs = static_cast<std::int32_t>(std::lround(
          std::chrono::duration<double, std::milli>(fenceWaitEnd - fenceWaitStart).count()));
      if (waitResult != VK_SUCCESS) {
        throw std::runtime_error("Failed to wait for frame scaffold fence");
      }

      releaseRetiredUiTextureResources(currentFrameSlot_);
      releaseRetiredBufferResources(currentFrameSlot_);

      const VkResult resetResult = vkResetFences(device_, 1, &sync.inFlight);
      if (resetResult != VK_SUCCESS) {
        throw std::runtime_error("Failed to reset frame scaffold fence");
      }
    }

    const auto acquireStart = std::chrono::steady_clock::now();
    const VkResult acquireResult = vkAcquireNextImageKHR(
        device_,
        swapchain_,
        UINT64_MAX,
        sync.imageAvailable,
        VK_NULL_HANDLE,
        &acquiredImageIndex_);
    const auto acquireEnd = std::chrono::steady_clock::now();
    latestFrameCpuTimings_.acquireMs = static_cast<std::int32_t>(std::lround(
      std::chrono::duration<double, std::milli>(acquireEnd - acquireStart).count()));

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
        prepareUiRetainedPanels(*meshData);
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

    const auto submitStart = std::chrono::steady_clock::now();
    const VkResult submitResult = vkQueueSubmit(graphicsQueue_, 1, &submitInfo, sync.inFlight);
    const auto submitEnd = std::chrono::steady_clock::now();
    latestFrameCpuTimings_.submitMs = static_cast<std::int32_t>(std::lround(
        std::chrono::duration<double, std::milli>(submitEnd - submitStart).count()));
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

    const auto presentStart = std::chrono::steady_clock::now();
    const VkResult presentResult = vkQueuePresentKHR(presentQueue_, &presentInfo);
    const auto presentEnd = std::chrono::steady_clock::now();
    latestFrameCpuTimings_.presentMs = static_cast<std::int32_t>(std::lround(
      std::chrono::duration<double, std::milli>(presentEnd - presentStart).count()));
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

  void VulkanRenderer::createUiOffscreenRenderPass()
  {
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = VK_FORMAT_R8G8B8A8_UNORM;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorAttachmentRef{};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorAttachmentRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &colorAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    if (vkCreateRenderPass(device_, &renderPassInfo, nullptr, &uiOffscreenRenderPass_) != VK_SUCCESS) {
      throw std::runtime_error("Failed to create UI offscreen render pass");
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

    VkPushConstantRange sdfPushConstantRange{};
    sdfPushConstantRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    sdfPushConstantRange.offset = 0;
    sdfPushConstantRange.size = sizeof(UiSdfPushConstants);
    sdfPushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &uiDescriptorSetLayout_;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &sdfPushConstantRange;

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
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = 256;

    if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &uiDescriptorPool_) != VK_SUCCESS) {
      throw std::runtime_error("Failed to create UI descriptor pool");
    }

    createUiFontComputeResources();
    createUiVectorTextComputeResources();

    createUiTextureResourceForKey("__white");
    const auto whiteIt = uiTextureResources_.find("__white");
    if (whiteIt != uiTextureResources_.end()) {
      uiDescriptorSet_ = whiteIt->second.descriptorSet;
      uiTextureImage_ = whiteIt->second.image;
      uiTextureImageMemory_ = whiteIt->second.imageMemory;
      uiTextureImageView_ = whiteIt->second.imageView;
    }
  }

  void VulkanRenderer::createUiFontComputeResources()
  {
    if (device_ == VK_NULL_HANDLE) {
      return;
    }

    VkDescriptorSetLayoutBinding curveBinding{};
    curveBinding.binding = 0;
    curveBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    curveBinding.descriptorCount = 1;
    curveBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutBinding jobBinding{};
    jobBinding.binding = 1;
    jobBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    jobBinding.descriptorCount = 1;
    jobBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutBinding atlasBinding{};
    atlasBinding.binding = 2;
    atlasBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    atlasBinding.descriptorCount = 1;
    atlasBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    const std::array<VkDescriptorSetLayoutBinding, 3> bindings = {
        curveBinding,
        jobBinding,
        atlasBinding,
    };

    VkDescriptorSetLayoutCreateInfo descriptorLayoutInfo{};
    descriptorLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptorLayoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
    descriptorLayoutInfo.pBindings = bindings.data();

    if (vkCreateDescriptorSetLayout(
            device_,
            &descriptorLayoutInfo,
            nullptr,
            &uiFontComputeDescriptorSetLayout_) != VK_SUCCESS) {
      throw std::runtime_error("Failed to create UI font compute descriptor set layout");
    }

    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(float);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &uiFontComputeDescriptorSetLayout_;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

    if (vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &uiFontComputePipelineLayout_) != VK_SUCCESS) {
      throw std::runtime_error("Failed to create UI font compute pipeline layout");
    }

    const std::array<VkDescriptorPoolSize, 2> poolSizes = {
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 32},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 16},
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = 16;

    if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &uiFontComputeDescriptorPool_) != VK_SUCCESS) {
      throw std::runtime_error("Failed to create UI font compute descriptor pool");
    }

    const auto computeShader = readBinaryFile("assets/shaders/msdfgen.comp.spv");
    if (!computeShader.has_value()) {
      VOLT_LOG_WARN_CAT(
          volt::core::logging::Category::kRender,
          "Missing UI font compute shader at assets/shaders/msdfgen.comp.spv");
      return;
    }

    VkShaderModule computeShaderModule = createShaderModule(computeShader.value());

    VkPipelineShaderStageCreateInfo stageInfo{};
    stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageInfo.module = computeShaderModule;
    stageInfo.pName = "main";

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = stageInfo;
    pipelineInfo.layout = uiFontComputePipelineLayout_;

    const VkResult pipelineResult = vkCreateComputePipelines(
        device_,
        VK_NULL_HANDLE,
        1,
        &pipelineInfo,
        nullptr,
        &uiFontComputePipeline_);

    vkDestroyShaderModule(device_, computeShaderModule, nullptr);

    if (pipelineResult != VK_SUCCESS) {
      throw std::runtime_error("Failed to create UI font compute pipeline");
    }
  }

  void VulkanRenderer::createUiVectorTextComputeResources()
  {
    if (device_ == VK_NULL_HANDLE) {
      return;
    }

    std::array<VkDescriptorSetLayoutBinding, 11> bindings{};
    for (std::uint32_t i = 0U; i < 10U; ++i) {
      bindings[i].binding = i;
      bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      bindings[i].descriptorCount = 1;
      bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    bindings[10].binding = 10;
    bindings[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[10].descriptorCount = 1;
    bindings[10].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo descriptorLayoutInfo{};
    descriptorLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptorLayoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
    descriptorLayoutInfo.pBindings = bindings.data();

    if (vkCreateDescriptorSetLayout(
            device_,
            &descriptorLayoutInfo,
            nullptr,
            &uiVectorTextComputeDescriptorSetLayout_) != VK_SUCCESS) {
      throw std::runtime_error("Failed to create UI vector text descriptor set layout");
    }

    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(UiVectorTextPushConstants);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &uiVectorTextComputeDescriptorSetLayout_;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

    if (vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &uiVectorTextComputePipelineLayout_) != VK_SUCCESS) {
      throw std::runtime_error("Failed to create UI vector text compute pipeline layout");
    }

    const std::array<VkDescriptorPoolSize, 2> poolSizes = {
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 256},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 32},
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = 32;

    if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &uiVectorTextComputeDescriptorPool_) != VK_SUCCESS) {
      throw std::runtime_error("Failed to create UI vector text compute descriptor pool");
    }

    // Prefix scan uses a dedicated descriptor layout (input counts, output offsets, output total).
    std::array<VkDescriptorSetLayoutBinding, 3> prefixScanBindings{};
    prefixScanBindings[0].binding = 0;
    prefixScanBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    prefixScanBindings[0].descriptorCount = 1;
    prefixScanBindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    prefixScanBindings[1].binding = 1;
    prefixScanBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    prefixScanBindings[1].descriptorCount = 1;
    prefixScanBindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    prefixScanBindings[2].binding = 2;
    prefixScanBindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    prefixScanBindings[2].descriptorCount = 1;
    prefixScanBindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo prefixScanLayoutInfo{};
    prefixScanLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    prefixScanLayoutInfo.bindingCount = static_cast<std::uint32_t>(prefixScanBindings.size());
    prefixScanLayoutInfo.pBindings = prefixScanBindings.data();

    if (vkCreateDescriptorSetLayout(
            device_,
            &prefixScanLayoutInfo,
            nullptr,
            &uiVectorTextPrefixScanDescriptorSetLayout_) != VK_SUCCESS) {
      throw std::runtime_error("Failed to create UI vector text prefix scan descriptor set layout");
    }

    VkPipelineLayoutCreateInfo prefixScanPipelineLayoutInfo{};
    prefixScanPipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    prefixScanPipelineLayoutInfo.setLayoutCount = 1;
    prefixScanPipelineLayoutInfo.pSetLayouts = &uiVectorTextPrefixScanDescriptorSetLayout_;
    prefixScanPipelineLayoutInfo.pushConstantRangeCount = 0;

    if (vkCreatePipelineLayout(device_, &prefixScanPipelineLayoutInfo, nullptr, &uiVectorTextPrefixScanPipelineLayout_) != VK_SUCCESS) {
      throw std::runtime_error("Failed to create UI vector text prefix scan pipeline layout");
    }

    const auto createComputePipeline = [&](const char* shaderPath, VkPipeline& outPipeline) {
      const auto shaderCode = readBinaryFile(shaderPath);
      if (!shaderCode.has_value()) {
        throw std::runtime_error(std::string("Missing UI vector text shader: ") + shaderPath);
      }

      VkShaderModule shaderModule = createShaderModule(shaderCode.value());

      VkPipelineShaderStageCreateInfo stageInfo{};
      stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
      stageInfo.module = shaderModule;
      stageInfo.pName = "main";

      VkComputePipelineCreateInfo pipelineInfo{};
      pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
      pipelineInfo.stage = stageInfo;
      pipelineInfo.layout = uiVectorTextComputePipelineLayout_;

      const VkResult result = vkCreateComputePipelines(
          device_,
          VK_NULL_HANDLE,
          1,
          &pipelineInfo,
          nullptr,
          &outPipeline);
      vkDestroyShaderModule(device_, shaderModule, nullptr);

       return result;
     };

     const auto createComputePipelineWithLayout = [&](const char* shaderPath, VkPipeline& outPipeline, VkPipelineLayout layout) {
       const auto shaderCode = readBinaryFile(shaderPath);
       if (!shaderCode.has_value()) {
         throw std::runtime_error(std::string("Missing UI vector text shader: ") + shaderPath);
       }

       VkShaderModule shaderModule = createShaderModule(shaderCode.value());

       VkPipelineShaderStageCreateInfo stageInfo{};
       stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
       stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
       stageInfo.module = shaderModule;
       stageInfo.pName = "main";

       VkComputePipelineCreateInfo pipelineInfo{};
       pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
       pipelineInfo.stage = stageInfo;
       pipelineInfo.layout = layout;

       const VkResult result = vkCreateComputePipelines(
           device_,
           VK_NULL_HANDLE,
           1,
           &pipelineInfo,
           nullptr,
           &outPipeline);
       vkDestroyShaderModule(device_, shaderModule, nullptr);
       return result;
    };
    createComputePipeline("assets/shaders/text/flatten_count.comp.spv", uiVectorTextFlattenCountPipeline_);
    createComputePipeline("assets/shaders/text/flatten_emit.comp.spv", uiVectorTextFlattenEmitPipeline_);
    createComputePipeline("assets/shaders/text/bin_count.comp.spv", uiVectorTextBinCountPipeline_);
    createComputePipeline("assets/shaders/text/bin_emit.comp.spv", uiVectorTextBinEmitPipeline_);
    createComputePipeline("assets/shaders/text/fine.comp.spv", uiVectorTextFinePipeline_);

      // Prefix scan uses its own layout (simpler - just input counts + output offsets)
      if (createComputePipelineWithLayout("assets/shaders/text/prefix_scan.comp.spv", uiVectorTextPrefixScanPipeline_, uiVectorTextPrefixScanPipelineLayout_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create prefix_scan pipeline");
      }

    // Exclusive scan (multi-workgroup): 3 SSBO bindings (counts/offsets in-place, block sums, params).
    std::array<VkDescriptorSetLayoutBinding, 3> excScanBindings{};
    for (std::uint32_t i = 0; i < 3; ++i) {
      excScanBindings[i].binding = i;
      excScanBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      excScanBindings[i].descriptorCount = 1;
      excScanBindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo excScanLayoutInfo{};
    excScanLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    excScanLayoutInfo.bindingCount = static_cast<std::uint32_t>(excScanBindings.size());
    excScanLayoutInfo.pBindings = excScanBindings.data();
    if (vkCreateDescriptorSetLayout(device_, &excScanLayoutInfo, nullptr, &uiVectorTextExclusiveScanDescriptorSetLayout_) != VK_SUCCESS) {
      throw std::runtime_error("Failed to create exclusive scan descriptor set layout");
    }

    VkPipelineLayoutCreateInfo excScanPipelineLayoutInfo{};
    excScanPipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    excScanPipelineLayoutInfo.setLayoutCount = 1;
    excScanPipelineLayoutInfo.pSetLayouts = &uiVectorTextExclusiveScanDescriptorSetLayout_;
    excScanPipelineLayoutInfo.pushConstantRangeCount = 0;
    if (vkCreatePipelineLayout(device_, &excScanPipelineLayoutInfo, nullptr, &uiVectorTextExclusiveScanPipelineLayout_) != VK_SUCCESS) {
      throw std::runtime_error("Failed to create exclusive scan pipeline layout");
    }

    if (createComputePipelineWithLayout("assets/shaders/text/exclusive_scan.comp.spv", uiVectorTextExclusiveScanPipeline_, uiVectorTextExclusiveScanPipelineLayout_) != VK_SUCCESS) {
      throw std::runtime_error("Failed to create exclusive_scan pipeline");
    }
  }

  void VulkanRenderer::createUiTextureResourceForKey(const std::string& textureKey)
  {
    if (textureKey.empty() || uiTextureResources_.find(textureKey) != uiTextureResources_.end()) {
      return;
    }

    if (textureKey == volt::io::defaultFontTextureKey()) {
      volt::io::FontGpuAtlas gpuAtlas{};
      if (volt::io::defaultFontGpuAtlas(gpuAtlas) && !gpuAtlas.jobs.empty() &&
          uiFontComputePipeline_ != VK_NULL_HANDLE &&
          uiFontComputeDescriptorSetLayout_ != VK_NULL_HANDLE &&
          uiFontComputePipelineLayout_ != VK_NULL_HANDLE &&
          uiFontComputeDescriptorPool_ != VK_NULL_HANDLE) {
        const VkFormat atlasFormat = findSupportedFormat(
            {VK_FORMAT_R16G16B16A16_SFLOAT},
            VK_IMAGE_TILING_OPTIMAL,
            VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);

        auto createHostStorageBuffer = [&](VkDeviceSize size,
                                           const void* sourceData,
                                           VkBuffer& outBuffer,
                                           VkDeviceMemory& outMemory) {
          VkBufferCreateInfo bufferInfo{};
          bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
          bufferInfo.size = size;
          bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
          bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

          if (vkCreateBuffer(device_, &bufferInfo, nullptr, &outBuffer) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create UI font compute buffer");
          }

          VkMemoryRequirements memoryRequirements{};
          vkGetBufferMemoryRequirements(device_, outBuffer, &memoryRequirements);

          VkMemoryAllocateInfo allocInfo{};
          allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
          allocInfo.allocationSize = memoryRequirements.size;
          allocInfo.memoryTypeIndex = findMemoryType(
              memoryRequirements.memoryTypeBits,
              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

          if (vkAllocateMemory(device_, &allocInfo, nullptr, &outMemory) != VK_SUCCESS) {
            throw std::runtime_error("Failed to allocate UI font compute buffer memory");
          }

          if (vkBindBufferMemory(device_, outBuffer, outMemory, 0) != VK_SUCCESS) {
            throw std::runtime_error("Failed to bind UI font compute buffer memory");
          }

          void* mapped = nullptr;
          if (vkMapMemory(device_, outMemory, 0, size, 0, &mapped) != VK_SUCCESS) {
            throw std::runtime_error("Failed to map UI font compute buffer memory");
          }
          std::memcpy(mapped, sourceData, static_cast<std::size_t>(size));
          vkUnmapMemory(device_, outMemory);
        };

        VkBuffer curveBuffer = VK_NULL_HANDLE;
        VkDeviceMemory curveBufferMemory = VK_NULL_HANDLE;
        createHostStorageBuffer(
            static_cast<VkDeviceSize>(gpuAtlas.curves.size() * sizeof(volt::io::FontGpuCurveSegment)),
            gpuAtlas.curves.data(),
            curveBuffer,
            curveBufferMemory);

        VkBuffer jobBuffer = VK_NULL_HANDLE;
        VkDeviceMemory jobBufferMemory = VK_NULL_HANDLE;
        createHostStorageBuffer(
            static_cast<VkDeviceSize>(gpuAtlas.jobs.size() * sizeof(volt::io::FontGpuGlyphJob)),
            gpuAtlas.jobs.data(),
            jobBuffer,
            jobBufferMemory);

        UiTextureResource resource{};

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = {gpuAtlas.atlasWidth, gpuAtlas.atlasHeight, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = atlasFormat;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateImage(device_, &imageInfo, nullptr, &resource.image) != VK_SUCCESS) {
          throw std::runtime_error("Failed to create GPU font atlas image");
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
          throw std::runtime_error("Failed to allocate GPU font atlas image memory");
        }

        if (vkBindImageMemory(device_, resource.image, resource.imageMemory, 0) != VK_SUCCESS) {
          throw std::runtime_error("Failed to bind GPU font atlas image memory");
        }

        VkImageViewCreateInfo imageViewInfo{};
        imageViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        imageViewInfo.image = resource.image;
        imageViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        imageViewInfo.format = atlasFormat;
        imageViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imageViewInfo.subresourceRange.baseMipLevel = 0;
        imageViewInfo.subresourceRange.levelCount = 1;
        imageViewInfo.subresourceRange.baseArrayLayer = 0;
        imageViewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device_, &imageViewInfo, nullptr, &resource.imageView) != VK_SUCCESS) {
          throw std::runtime_error("Failed to create GPU font atlas image view");
        }

        VkDescriptorSetAllocateInfo computeSetAllocInfo{};
        computeSetAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        computeSetAllocInfo.descriptorPool = uiFontComputeDescriptorPool_;
        computeSetAllocInfo.descriptorSetCount = 1;
        computeSetAllocInfo.pSetLayouts = &uiFontComputeDescriptorSetLayout_;

        VkDescriptorSet computeDescriptorSet = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(device_, &computeSetAllocInfo, &computeDescriptorSet) != VK_SUCCESS) {
          throw std::runtime_error("Failed to allocate UI font compute descriptor set");
        }

        const VkDescriptorBufferInfo curveBufferInfo{
            curveBuffer,
            0,
            static_cast<VkDeviceSize>(gpuAtlas.curves.size() * sizeof(volt::io::FontGpuCurveSegment)),
        };
        const VkDescriptorBufferInfo jobBufferInfo{
            jobBuffer,
            0,
            static_cast<VkDeviceSize>(gpuAtlas.jobs.size() * sizeof(volt::io::FontGpuGlyphJob)),
        };
        const VkDescriptorImageInfo storageImageInfo{
            VK_NULL_HANDLE,
            resource.imageView,
            VK_IMAGE_LAYOUT_GENERAL,
        };

        const std::array<VkWriteDescriptorSet, 3> computeWrites = {
            VkWriteDescriptorSet{
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                nullptr,
                computeDescriptorSet,
                0,
                0,
                1,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                nullptr,
                &curveBufferInfo,
                nullptr,
            },
            VkWriteDescriptorSet{
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                nullptr,
                computeDescriptorSet,
                1,
                0,
                1,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                nullptr,
                &jobBufferInfo,
                nullptr,
            },
            VkWriteDescriptorSet{
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                nullptr,
                computeDescriptorSet,
                2,
                0,
                1,
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                &storageImageInfo,
                nullptr,
                nullptr,
            },
        };
        vkUpdateDescriptorSets(
            device_,
            static_cast<std::uint32_t>(computeWrites.size()),
            computeWrites.data(),
            0,
            nullptr);

        VkCommandBufferAllocateInfo commandAllocInfo{};
        commandAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        commandAllocInfo.commandPool = scaffoldCommandPool_;
        commandAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandAllocInfo.commandBufferCount = 1;

        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(device_, &commandAllocInfo, &commandBuffer) != VK_SUCCESS) {
          throw std::runtime_error("Failed to allocate GPU font atlas command buffer");
        }

        VkFence computeFence = VK_NULL_HANDLE;
        VkFenceCreateInfo computeFenceInfo{};
        computeFenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(device_, &computeFenceInfo, nullptr, &computeFence) != VK_SUCCESS) {
          vkFreeCommandBuffers(device_, scaffoldCommandPool_, 1, &commandBuffer);
          throw std::runtime_error("Failed to create GPU font atlas compute fence");
        }

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(commandBuffer, &beginInfo);

        VkImageMemoryBarrier toGeneral{};
        toGeneral.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toGeneral.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral.image = resource.image;
        toGeneral.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toGeneral.subresourceRange.baseMipLevel = 0;
        toGeneral.subresourceRange.levelCount = 1;
        toGeneral.subresourceRange.baseArrayLayer = 0;
        toGeneral.subresourceRange.layerCount = 1;
        toGeneral.srcAccessMask = 0;
        toGeneral.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;

        vkCmdPipelineBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0,
            0,
            nullptr,
            0,
            nullptr,
            1,
            &toGeneral);

          VkClearColorValue clearColor{};
          clearColor.float32[0] = 0.0F;
          clearColor.float32[1] = 0.0F;
          clearColor.float32[2] = 0.0F;
          clearColor.float32[3] = 0.0F;
          VkImageSubresourceRange clearRange{};
          clearRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
          clearRange.baseMipLevel = 0;
          clearRange.levelCount = 1;
          clearRange.baseArrayLayer = 0;
          clearRange.layerCount = 1;
          vkCmdClearColorImage(
            commandBuffer,
            resource.image,
            VK_IMAGE_LAYOUT_GENERAL,
            &clearColor,
            1,
            &clearRange);

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, uiFontComputePipeline_);
        vkCmdBindDescriptorSets(
            commandBuffer,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            uiFontComputePipelineLayout_,
            0,
            1,
            &computeDescriptorSet,
            0,
            nullptr);

        const float spreadPx = std::max(1.0F, gpuAtlas.sdfSpreadPx);
        vkCmdPushConstants(
            commandBuffer,
            uiFontComputePipelineLayout_,
            VK_SHADER_STAGE_COMPUTE_BIT,
            0,
            sizeof(float),
            &spreadPx);

        const std::uint32_t groupCountX = std::max(1U, (gpuAtlas.maxGlyphWidth + 7U) / 8U);
        const std::uint32_t groupCountY = std::max(1U, (gpuAtlas.maxGlyphHeight + 7U) / 8U);
        const std::uint32_t groupCountZ = static_cast<std::uint32_t>(gpuAtlas.jobs.size());
        vkCmdDispatch(commandBuffer, groupCountX, groupCountY, groupCountZ);

        VkImageMemoryBarrier toSampled{};
        toSampled.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toSampled.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        toSampled.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toSampled.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSampled.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSampled.image = resource.image;
        toSampled.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toSampled.subresourceRange.baseMipLevel = 0;
        toSampled.subresourceRange.levelCount = 1;
        toSampled.subresourceRange.baseArrayLayer = 0;
        toSampled.subresourceRange.layerCount = 1;
        toSampled.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        toSampled.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
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
        bool gpuAtlasComputeFailed = false;
        if (vkQueueSubmit(graphicsQueue_, 1, &submitInfo, computeFence) != VK_SUCCESS) {
          gpuAtlasComputeFailed = true;
          VOLT_LOG_WARN_CAT(
              volt::core::logging::Category::kRender,
              "GPU font atlas submit failed; falling back to staged image upload");
        } else if (vkWaitForFences(device_, 1, &computeFence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
          gpuAtlasComputeFailed = true;
          VOLT_LOG_WARN_CAT(
              volt::core::logging::Category::kRender,
              "GPU font atlas fence wait failed; falling back to staged image upload");
        }

        if (gpuAtlasComputeFailed) {
          vkQueueWaitIdle(graphicsQueue_);
        }

        if (gpuAtlasComputeFailed) {
          if (computeDescriptorSet != VK_NULL_HANDLE) {
            vkFreeDescriptorSets(device_, uiFontComputeDescriptorPool_, 1, &computeDescriptorSet);
            computeDescriptorSet = VK_NULL_HANDLE;
          }
          if (computeFence != VK_NULL_HANDLE) {
            vkDestroyFence(device_, computeFence, nullptr);
            computeFence = VK_NULL_HANDLE;
          }
          if (commandBuffer != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(device_, scaffoldCommandPool_, 1, &commandBuffer);
            commandBuffer = VK_NULL_HANDLE;
          }
          retireBufferResource(curveBuffer, curveBufferMemory, "ui-font-curve-buffer");
          retireBufferResource(jobBuffer, jobBufferMemory, "ui-font-job-buffer");
          if (resource.imageView != VK_NULL_HANDLE) {
            vkDestroyImageView(device_, resource.imageView, nullptr);
            resource.imageView = VK_NULL_HANDLE;
          }
          if (resource.image != VK_NULL_HANDLE) {
            vkDestroyImage(device_, resource.image, nullptr);
            resource.image = VK_NULL_HANDLE;
          }
          if (resource.imageMemory != VK_NULL_HANDLE) {
            vkFreeMemory(device_, resource.imageMemory, nullptr);
            resource.imageMemory = VK_NULL_HANDLE;
          }
        } else {

          if (vkResetCommandBuffer(commandBuffer, 0) != VK_SUCCESS) {
            vkFreeDescriptorSets(device_, uiFontComputeDescriptorPool_, 1, &computeDescriptorSet);
            vkDestroyFence(device_, computeFence, nullptr);
            vkFreeCommandBuffers(device_, scaffoldCommandPool_, 1, &commandBuffer);
            retireBufferResource(curveBuffer, curveBufferMemory, "ui-font-curve-buffer");
            retireBufferResource(jobBuffer, jobBufferMemory, "ui-font-job-buffer");
            throw std::runtime_error("Failed to reset GPU font atlas command buffer after execution");
          }

          vkFreeDescriptorSets(device_, uiFontComputeDescriptorPool_, 1, &computeDescriptorSet);
          vkDestroyFence(device_, computeFence, nullptr);
          vkFreeCommandBuffers(device_, scaffoldCommandPool_, 1, &commandBuffer);
          retireBufferResource(curveBuffer, curveBufferMemory, "ui-font-curve-buffer");
          retireBufferResource(jobBuffer, jobBufferMemory, "ui-font-job-buffer");

          VkDescriptorSetAllocateInfo setAllocInfo{};
          setAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
          setAllocInfo.descriptorPool = uiDescriptorPool_;
          setAllocInfo.descriptorSetCount = 1;
          setAllocInfo.pSetLayouts = &uiDescriptorSetLayout_;

          if (vkAllocateDescriptorSets(device_, &setAllocInfo, &resource.descriptorSet) != VK_SUCCESS) {
            throw std::runtime_error("Failed to allocate GPU font atlas descriptor set");
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

          VOLT_LOG_DEBUG_CAT(
              volt::core::logging::Category::kRender,
              "Generated GPU font atlas: size=",
              gpuAtlas.atlasWidth,
              "x",
              gpuAtlas.atlasHeight,
              " curves=",
              gpuAtlas.curves.size(),
              " glyphJobs=",
              gpuAtlas.jobs.size());

          resource.fontAtlasRevision = volt::io::defaultFontAtlasRevision();
          uiTextureResources_.emplace(textureKey, resource);
          return;
        }
      }
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

    VkFence uploadFence = VK_NULL_HANDLE;
    VkFenceCreateInfo uploadFenceInfo{};
    uploadFenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(device_, &uploadFenceInfo, nullptr, &uploadFence) != VK_SUCCESS) {
      vkFreeCommandBuffers(device_, scaffoldCommandPool_, 1, &commandBuffer);
      throw std::runtime_error("Failed to create UI keyed texture upload fence");
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
    if (vkQueueSubmit(graphicsQueue_, 1, &submitInfo, uploadFence) != VK_SUCCESS) {
      vkDestroyFence(device_, uploadFence, nullptr);
      throw std::runtime_error("Failed to submit UI keyed texture upload command buffer");
    }

    if (vkWaitForFences(device_, 1, &uploadFence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
      vkDestroyFence(device_, uploadFence, nullptr);
      throw std::runtime_error("Failed to wait for UI keyed texture upload fence");
    }

    if (vkResetCommandBuffer(commandBuffer, 0) != VK_SUCCESS) {
      vkDestroyFence(device_, uploadFence, nullptr);
      throw std::runtime_error("Failed to reset UI keyed texture upload command buffer");
    }

    vkDestroyFence(device_, uploadFence, nullptr);
    vkFreeCommandBuffers(device_, scaffoldCommandPool_, 1, &commandBuffer);
    retireBufferResource(stagingBuffer, stagingBufferMemory, "ui-keyed-texture-staging");

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

    if (textureKey == volt::io::defaultFontTextureKey()) {
      resource.fontAtlasRevision = volt::io::defaultFontAtlasRevision();
    }

    uiTextureResources_.emplace(textureKey, resource);
  }

  void VulkanRenderer::releaseUiTextureResource(UiTextureResource& resource)
  {
    if (resource.framebuffer != VK_NULL_HANDLE) {
      vkDestroyFramebuffer(device_, resource.framebuffer, nullptr);
      resource.framebuffer = VK_NULL_HANDLE;
    }

    if (resource.imageView != VK_NULL_HANDLE) {
      vkDestroyImageView(device_, resource.imageView, nullptr);
      resource.imageView = VK_NULL_HANDLE;
    }
    if (resource.image != VK_NULL_HANDLE) {
      vkDestroyImage(device_, resource.image, nullptr);
      resource.image = VK_NULL_HANDLE;
    }
    if (resource.imageMemory != VK_NULL_HANDLE) {
      vkFreeMemory(device_, resource.imageMemory, nullptr);
      resource.imageMemory = VK_NULL_HANDLE;
    }
    if (resource.descriptorSet != VK_NULL_HANDLE && uiDescriptorPool_ != VK_NULL_HANDLE) {
      vkFreeDescriptorSets(device_, uiDescriptorPool_, 1, &resource.descriptorSet);
      resource.descriptorSet = VK_NULL_HANDLE;
    }
  }

  void VulkanRenderer::releaseRetiredUiTextureResources(std::uint32_t frameSlot)
  {
    if (frameSlot >= retiredUiTextureResources_.size()) {
      return;
    }

    for (UiTextureResource& resource : retiredUiTextureResources_[frameSlot]) {
      releaseUiTextureResource(resource);
    }
    retiredUiTextureResources_[frameSlot].clear();
  }

  void VulkanRenderer::retireBufferResource(VkBuffer& buffer, VkDeviceMemory& memory, const char* label)
  {
    if (buffer == VK_NULL_HANDLE && memory == VK_NULL_HANDLE) {
      return;
    }

    if (device_ == VK_NULL_HANDLE) {
      if (buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, buffer, nullptr);
      }
      if (memory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, memory, nullptr);
      }
    } else {
      retiredBufferResources_[currentFrameSlot_].push_back({buffer, memory, label});
    }

    buffer = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
  }

  void VulkanRenderer::releaseRetiredBufferResources(std::uint32_t frameSlot)
  {
    if (frameSlot >= retiredBufferResources_.size()) {
      return;
    }

    for (RetiredBufferResource& resource : retiredBufferResources_[frameSlot]) {
      if (resource.buffer != VK_NULL_HANDLE && resource.label != nullptr) {
        VOLT_LOG_INFO_CAT(
            volt::core::logging::Category::kRender,
            "Releasing retired buffer '{}' handle={} on frame slot {}",
            resource.label,
            static_cast<const void*>(resource.buffer),
            frameSlot);
      }
      if (resource.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, resource.buffer, nullptr);
      }
      if (resource.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, resource.memory, nullptr);
      }
    }
    retiredBufferResources_[frameSlot].clear();
  }

  void VulkanRenderer::destroyUiTextureResource(const std::string& textureKey, bool deferDestruction)
  {
    const auto it = uiTextureResources_.find(textureKey);
    if (it == uiTextureResources_.end()) {
      return;
    }

    UiTextureResource resource = std::move(it->second);
    uiTextureResources_.erase(it);

    if (!deferDestruction || device_ == VK_NULL_HANDLE) {
      releaseUiTextureResource(resource);
      return;
    }

    retiredUiTextureResources_[currentFrameSlot_].push_back(std::move(resource));
  }

  void VulkanRenderer::cleanupUiVectorTextTransientResources()
  {
    for (const std::string& textureKey : uiVectorTextTransientKeys_) {
      destroyUiTextureResource(textureKey);
      unresolvedTextureKeysLogged_.erase(textureKey);
    }
    uiVectorTextTransientKeys_.clear();
  }

  void VulkanRenderer::prepareUiVectorTextBatches(const volt::ui::UiMeshData& meshData)
  {
    std::unordered_set<std::string> activeVectorTextKeys;
    activeVectorTextKeys.reserve(meshData.vectorTextBatches.size());

    if (meshData.vectorTextBatches.empty()) {
      cleanupUiVectorTextTransientResources();
      return;
    }

    for (const volt::ui::UiVectorTextBatch& batch : meshData.vectorTextBatches) {
      activeVectorTextKeys.insert(batch.textureKey);
      std::string error;
      if (!renderUiVectorTextBatchToTexture(batch, error)) {
        VOLT_LOG_WARN_CAT(
            volt::core::logging::Category::kRender,
            "Vector text batch render failed for widget ",
            batch.widgetId,
            ": ",
            error);
      }
    }

    for (auto it = uiVectorTextTransientKeys_.begin(); it != uiVectorTextTransientKeys_.end();) {
      if (activeVectorTextKeys.find(*it) != activeVectorTextKeys.end()) {
        ++it;
        continue;
      }

      destroyUiTextureResource(*it);
      unresolvedTextureKeysLogged_.erase(*it);
      it = uiVectorTextTransientKeys_.erase(it);
    }
  }

  bool VulkanRenderer::renderUiVectorTextBatchToTexture(const volt::ui::UiVectorTextBatch& batch, std::string& outError)
  {
    outError.clear();

    if (batch.curves.empty()) {
      outError = "vector text batch has no curves";
      return false;
    }

    if (uiVectorTextComputePipelineLayout_ == VK_NULL_HANDLE ||
        uiVectorTextComputeDescriptorSetLayout_ == VK_NULL_HANDLE ||
        uiVectorTextComputeDescriptorPool_ == VK_NULL_HANDLE ||
        uiVectorTextFlattenCountPipeline_ == VK_NULL_HANDLE ||
        uiVectorTextFlattenEmitPipeline_ == VK_NULL_HANDLE ||
        uiVectorTextBinCountPipeline_ == VK_NULL_HANDLE ||
        uiVectorTextBinEmitPipeline_ == VK_NULL_HANDLE ||
        uiVectorTextFinePipeline_ == VK_NULL_HANDLE) {
      outError = "vector text compute pipelines are not initialized";
      return false;
    }

    const std::uint64_t atlasRevision = volt::io::defaultFontAtlasRevision();
    auto hashCombine = [](std::uint64_t seed, std::uint64_t value) {
      seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
      return seed;
    };
    auto hashFloat = [](float value) -> std::uint64_t {
      std::uint32_t bits = 0U;
      static_assert(sizeof(bits) == sizeof(value));
      std::memcpy(&bits, &value, sizeof(bits));
      return static_cast<std::uint64_t>(bits);
    };

    std::uint64_t batchSignature = 0U;
    batchSignature = hashCombine(batchSignature, static_cast<std::uint64_t>(batch.widgetId));
    batchSignature = hashCombine(batchSignature, static_cast<std::uint64_t>(batch.imageWidth));
    batchSignature = hashCombine(batchSignature, static_cast<std::uint64_t>(batch.imageHeight));
    batchSignature = hashCombine(batchSignature, static_cast<std::uint64_t>(batch.curves.size()));
    for (const volt::ui::UiVectorCurveInput& curve : batch.curves) {
      batchSignature = hashCombine(batchSignature, hashFloat(curve.p0x));
      batchSignature = hashCombine(batchSignature, hashFloat(curve.p0y));
      batchSignature = hashCombine(batchSignature, hashFloat(curve.p1x));
      batchSignature = hashCombine(batchSignature, hashFloat(curve.p1y));
      batchSignature = hashCombine(batchSignature, hashFloat(curve.p2x));
      batchSignature = hashCombine(batchSignature, hashFloat(curve.p2y));
      batchSignature = hashCombine(batchSignature, hashFloat(curve.p3x));
      batchSignature = hashCombine(batchSignature, hashFloat(curve.p3y));
      batchSignature = hashCombine(batchSignature, static_cast<std::uint64_t>(curve.pathId));
      batchSignature = hashCombine(batchSignature, static_cast<std::uint64_t>(curve.isCubic));
    }

    const auto existingIt = uiTextureResources_.find(batch.textureKey);
    if (existingIt != uiTextureResources_.end() &&
        existingIt->second.fontAtlasRevision == atlasRevision &&
        existingIt->second.vectorTextSignature == batchSignature) {
      uiVectorTextTransientKeys_.insert(batch.textureKey);
      return true;
    }

    destroyUiTextureResource(batch.textureKey);

    const std::uint32_t imageWidth = std::max(1U, batch.imageWidth);
    const std::uint32_t imageHeight = std::max(1U, batch.imageHeight);
    const std::uint32_t curveCount = static_cast<std::uint32_t>(batch.curves.size());
    const std::uint32_t tilesWide = std::max(1U, (imageWidth + kVectorTextTileSize - 1U) / kVectorTextTileSize);
    const std::uint32_t tilesHigh = std::max(1U, (imageHeight + kVectorTextTileSize - 1U) / kVectorTextTileSize);
    const std::uint32_t tileCount = tilesWide * tilesHigh;

    struct GpuLineSegment {
      float p0x{0.0F};
      float p0y{0.0F};
      float p1x{0.0F};
      float p1y{0.0F};
      std::uint32_t pathId{0};
      std::uint32_t curveIndex{0};
    };

    struct GpuCurveMeta {
      std::uint32_t segmentOffset{0};
      std::uint32_t segmentCount{0};
    };

    struct TempBuffer {
      VkBuffer buffer{VK_NULL_HANDLE};
      VkDeviceMemory memory{VK_NULL_HANDLE};
      VkDeviceSize size{0};
    };

    auto destroyTempBuffer = [&](TempBuffer& tempBuffer) {
      retireBufferResource(tempBuffer.buffer, tempBuffer.memory);
      tempBuffer.size = 0;
    };

    auto createHostVisibleBuffer = [&](VkDeviceSize requestedSize, TempBuffer& outBuffer, const void* initialData) -> bool {
      const VkDeviceSize bufferSize = std::max<VkDeviceSize>(requestedSize, 4);

      VkBufferCreateInfo bufferInfo{};
      bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
      bufferInfo.size = bufferSize;
      bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
      bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      if (vkCreateBuffer(device_, &bufferInfo, nullptr, &outBuffer.buffer) != VK_SUCCESS) {
        outError = "failed to create vector text storage buffer";
        return false;
      }

      VkMemoryRequirements memoryRequirements{};
      vkGetBufferMemoryRequirements(device_, outBuffer.buffer, &memoryRequirements);

      VkMemoryAllocateInfo allocInfo{};
      allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      allocInfo.allocationSize = memoryRequirements.size;
      allocInfo.memoryTypeIndex = findMemoryType(
          memoryRequirements.memoryTypeBits,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      if (vkAllocateMemory(device_, &allocInfo, nullptr, &outBuffer.memory) != VK_SUCCESS) {
        outError = "failed to allocate vector text storage memory";
        return false;
      }

      if (vkBindBufferMemory(device_, outBuffer.buffer, outBuffer.memory, 0) != VK_SUCCESS) {
        outError = "failed to bind vector text storage memory";
        return false;
      }

      void* mapped = nullptr;
      if (vkMapMemory(device_, outBuffer.memory, 0, bufferSize, 0, &mapped) != VK_SUCCESS) {
        outError = "failed to map vector text storage memory";
        return false;
      }
      std::memset(mapped, 0, static_cast<std::size_t>(bufferSize));
      if (initialData != nullptr && requestedSize > 0) {
        std::memcpy(mapped, initialData, static_cast<std::size_t>(requestedSize));
      }
      vkUnmapMemory(device_, outBuffer.memory);
      outBuffer.size = bufferSize;
      return true;
    };

    auto readUintBuffer = [&](const TempBuffer& sourceBuffer, std::size_t count, std::vector<std::uint32_t>& outValues) -> bool {
      outValues.assign(count, 0U);
      if (count == 0U) {
        return true;
      }

      void* mapped = nullptr;
      if (vkMapMemory(device_, sourceBuffer.memory, 0, sourceBuffer.size, 0, &mapped) != VK_SUCCESS) {
        outError = "failed to map vector text uint buffer for read";
        return false;
      }
      std::memcpy(outValues.data(), mapped, count * sizeof(std::uint32_t));
      vkUnmapMemory(device_, sourceBuffer.memory);
      return true;
    };

    auto writeUintBuffer = [&](const TempBuffer& destinationBuffer, const std::vector<std::uint32_t>& values) -> bool {
      void* mapped = nullptr;
      if (vkMapMemory(device_, destinationBuffer.memory, 0, destinationBuffer.size, 0, &mapped) != VK_SUCCESS) {
        outError = "failed to map vector text uint buffer for write";
        return false;
      }
      std::memset(mapped, 0, static_cast<std::size_t>(destinationBuffer.size));
      if (!values.empty()) {
        std::memcpy(mapped, values.data(), values.size() * sizeof(std::uint32_t));
      }
      vkUnmapMemory(device_, destinationBuffer.memory);
      return true;
    };

    VkCommandBuffer reusableComputeCommandBuffer = VK_NULL_HANDLE;
    VkFence reusableComputeFence = VK_NULL_HANDLE;
    VkQueryPool vectorTextTimestampQueryPool = VK_NULL_HANDLE;
    bool vectorTextTimestampResetPending = false;

    VkCommandBufferAllocateInfo reusableAllocInfo{};
    reusableAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    reusableAllocInfo.commandPool = scaffoldCommandPool_;
    reusableAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    reusableAllocInfo.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device_, &reusableAllocInfo, &reusableComputeCommandBuffer) != VK_SUCCESS) {
      outError = "failed to allocate reusable vector text command buffer";
      return false;
    }

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(device_, &fenceInfo, nullptr, &reusableComputeFence) != VK_SUCCESS) {
      vkFreeCommandBuffers(device_, scaffoldCommandPool_, 1, &reusableComputeCommandBuffer);
      reusableComputeCommandBuffer = VK_NULL_HANDLE;
      outError = "failed to create vector text reusable fence";
      return false;
    }

    constexpr std::uint32_t kVectorTextTimestampCount = 10;
    VkQueryPoolCreateInfo timestampPoolInfo{};
    timestampPoolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    timestampPoolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    timestampPoolInfo.queryCount = kVectorTextTimestampCount;
    if (vkCreateQueryPool(device_, &timestampPoolInfo, nullptr, &vectorTextTimestampQueryPool) == VK_SUCCESS) {
      vectorTextTimestampResetPending = true;
    }

    auto submitComputeWork = [&](const std::function<void(VkCommandBuffer)>& recordCommands,
                                 std::uint32_t beginTimestampQuery = UINT32_MAX,
                                 std::uint32_t endTimestampQuery = UINT32_MAX) -> bool {
      if (reusableComputeCommandBuffer == VK_NULL_HANDLE || reusableComputeFence == VK_NULL_HANDLE) {
        outError = "vector text reusable command resources are not initialized";
        return false;
      }

      if (vkResetCommandBuffer(reusableComputeCommandBuffer, 0) != VK_SUCCESS) {
        outError = "failed to reset vector text command buffer";
        return false;
      }
      VkCommandBufferBeginInfo beginInfo{};
      beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      if (vkBeginCommandBuffer(reusableComputeCommandBuffer, &beginInfo) != VK_SUCCESS) {
        outError = "failed to begin vector text command buffer";
        return false;
      }

      if (vectorTextTimestampQueryPool != VK_NULL_HANDLE && vectorTextTimestampResetPending) {
        vkCmdResetQueryPool(reusableComputeCommandBuffer, vectorTextTimestampQueryPool, 0, kVectorTextTimestampCount);
        vectorTextTimestampResetPending = false;
      }

      if (vectorTextTimestampQueryPool != VK_NULL_HANDLE && beginTimestampQuery < kVectorTextTimestampCount) {
        vkCmdWriteTimestamp(
            reusableComputeCommandBuffer,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            vectorTextTimestampQueryPool,
            beginTimestampQuery);
      }

      recordCommands(reusableComputeCommandBuffer);

      if (vectorTextTimestampQueryPool != VK_NULL_HANDLE && endTimestampQuery < kVectorTextTimestampCount) {
        vkCmdWriteTimestamp(
            reusableComputeCommandBuffer,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            vectorTextTimestampQueryPool,
            endTimestampQuery);
      }

      if (vkEndCommandBuffer(reusableComputeCommandBuffer) != VK_SUCCESS) {
        outError = "failed to end vector text command buffer";
        return false;
      }

      if (vkResetFences(device_, 1, &reusableComputeFence) != VK_SUCCESS) {
        outError = "failed to reset vector text compute fence";
        return false;
      }

      VkSubmitInfo submitInfo{};
      submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      submitInfo.commandBufferCount = 1;
      submitInfo.pCommandBuffers = &reusableComputeCommandBuffer;
      if (vkQueueSubmit(graphicsQueue_, 1, &submitInfo, reusableComputeFence) != VK_SUCCESS) {
        outError = "failed to submit vector text command buffer";
        return false;
      }

      if (vkWaitForFences(device_, 1, &reusableComputeFence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        outError = "failed to wait for vector text compute fence";
        return false;
      }

      if (vkResetCommandBuffer(reusableComputeCommandBuffer, 0) != VK_SUCCESS) {
        outError = "failed to reset vector text command buffer after execution";
        return false;
      }
      return true;
    };

    auto copyBufferOnGpu = [&](const TempBuffer& sourceBuffer, const TempBuffer& destinationBuffer, VkDeviceSize byteSize) -> bool {
      if (byteSize == 0) {
        return true;
      }

      return submitComputeWork([&](VkCommandBuffer commandBuffer) {
        VkBufferCopy copyRegion{};
        copyRegion.srcOffset = 0;
        copyRegion.dstOffset = 0;
        copyRegion.size = byteSize;
        vkCmdCopyBuffer(commandBuffer, sourceBuffer.buffer, destinationBuffer.buffer, 1, &copyRegion);
      });
    };

    TempBuffer curveBuffer{};
    TempBuffer curveCountBuffer{};
    TempBuffer curveOffsetBuffer{};
    TempBuffer segmentBuffer{};
    TempBuffer curveMetaBuffer{};
    TempBuffer tileCountBuffer{};
    TempBuffer tileOffsetBuffer{};
    TempBuffer tileFillPtrBuffer{};
    TempBuffer tileSegmentListBuffer{};
    TempBuffer backdropBuffer{};
    TempBuffer prefixScanTotalBuffer{};
    TempBuffer exclusiveScanParamsBuffer{};
    TempBuffer exclusiveScanBlockSumsBuffer{};
    TempBuffer exclusiveScanBlockSumsScratchBuffer{};
    VkDescriptorSet computeDescriptorSet = VK_NULL_HANDLE;
    VkDescriptorSet prefixScanDescriptorSet = VK_NULL_HANDLE;
    VkDescriptorSet exclusiveScanDescriptorSet = VK_NULL_HANDLE;

    UiTextureResource resource{};
    resource.fontAtlasRevision = atlasRevision;
    resource.vectorTextSignature = batchSignature;

    auto cleanup = [&]() {
      if (vectorTextTimestampQueryPool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(device_, vectorTextTimestampQueryPool, nullptr);
        vectorTextTimestampQueryPool = VK_NULL_HANDLE;
      }
      if (reusableComputeFence != VK_NULL_HANDLE) {
        vkDestroyFence(device_, reusableComputeFence, nullptr);
        reusableComputeFence = VK_NULL_HANDLE;
      }
      if (reusableComputeCommandBuffer != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(device_, scaffoldCommandPool_, 1, &reusableComputeCommandBuffer);
        reusableComputeCommandBuffer = VK_NULL_HANDLE;
      }
      if (computeDescriptorSet != VK_NULL_HANDLE) {
        vkFreeDescriptorSets(device_, uiVectorTextComputeDescriptorPool_, 1, &computeDescriptorSet);
        computeDescriptorSet = VK_NULL_HANDLE;
      }
      if (prefixScanDescriptorSet != VK_NULL_HANDLE) {
        vkFreeDescriptorSets(device_, uiVectorTextComputeDescriptorPool_, 1, &prefixScanDescriptorSet);
        prefixScanDescriptorSet = VK_NULL_HANDLE;
      }
      if (exclusiveScanDescriptorSet != VK_NULL_HANDLE) {
        vkFreeDescriptorSets(device_, uiVectorTextComputeDescriptorPool_, 1, &exclusiveScanDescriptorSet);
        exclusiveScanDescriptorSet = VK_NULL_HANDLE;
      }
      destroyTempBuffer(prefixScanTotalBuffer);
      destroyTempBuffer(exclusiveScanParamsBuffer);
      destroyTempBuffer(exclusiveScanBlockSumsBuffer);
      destroyTempBuffer(exclusiveScanBlockSumsScratchBuffer);
      destroyTempBuffer(backdropBuffer);
      destroyTempBuffer(tileSegmentListBuffer);
      destroyTempBuffer(tileFillPtrBuffer);
      destroyTempBuffer(tileOffsetBuffer);
      destroyTempBuffer(tileCountBuffer);
      destroyTempBuffer(curveMetaBuffer);
      destroyTempBuffer(segmentBuffer);
      destroyTempBuffer(curveOffsetBuffer);
      destroyTempBuffer(curveCountBuffer);
      destroyTempBuffer(curveBuffer);
      if (resource.imageView != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, resource.imageView, nullptr);
        resource.imageView = VK_NULL_HANDLE;
      }
      if (resource.image != VK_NULL_HANDLE) {
        vkDestroyImage(device_, resource.image, nullptr);
        resource.image = VK_NULL_HANDLE;
      }
      if (resource.imageMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, resource.imageMemory, nullptr);
        resource.imageMemory = VK_NULL_HANDLE;
      }
    };

    if (!createHostVisibleBuffer(
            static_cast<VkDeviceSize>(batch.curves.size() * sizeof(volt::ui::UiVectorCurveInput)),
            curveBuffer,
            batch.curves.data()) ||
        !createHostVisibleBuffer(static_cast<VkDeviceSize>(curveCount * sizeof(std::uint32_t)), curveCountBuffer, nullptr) ||
        !createHostVisibleBuffer(static_cast<VkDeviceSize>(curveCount * sizeof(std::uint32_t)), curveOffsetBuffer, nullptr) ||
        !createHostVisibleBuffer(static_cast<VkDeviceSize>(tileCount * sizeof(std::uint32_t)), tileCountBuffer, nullptr) ||
        !createHostVisibleBuffer(static_cast<VkDeviceSize>(tileCount * sizeof(std::uint32_t)), tileOffsetBuffer, nullptr) ||
        !createHostVisibleBuffer(static_cast<VkDeviceSize>(tileCount * sizeof(std::uint32_t)), tileFillPtrBuffer, nullptr) ||
        !createHostVisibleBuffer(static_cast<VkDeviceSize>(tileCount * sizeof(std::int32_t)), backdropBuffer, nullptr) ||
        !createHostVisibleBuffer(sizeof(std::uint32_t), prefixScanTotalBuffer, nullptr) ||
        !createHostVisibleBuffer(2U * sizeof(std::uint32_t), exclusiveScanParamsBuffer, nullptr) ||
        !createHostVisibleBuffer(
            static_cast<VkDeviceSize>(
                std::max(1U, std::max((curveCount + 255U) / 256U, (tileCount + 255U) / 256U)) *
                sizeof(std::uint32_t)),
            exclusiveScanBlockSumsBuffer,
          nullptr) ||
        !createHostVisibleBuffer(
          static_cast<VkDeviceSize>(
            std::max(1U, std::max((curveCount + 255U) / 256U, (tileCount + 255U) / 256U)) *
            sizeof(std::uint32_t)),
          exclusiveScanBlockSumsScratchBuffer,
            nullptr)) {
      cleanup();
      return false;
    }

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {imageWidth, imageHeight, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(device_, &imageInfo, nullptr, &resource.image) != VK_SUCCESS) {
      cleanup();
      outError = "failed to create vector text image";
      return false;
    }

    VkMemoryRequirements imageMemoryRequirements{};
    vkGetImageMemoryRequirements(device_, resource.image, &imageMemoryRequirements);
    VkMemoryAllocateInfo imageAllocInfo{};
    imageAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    imageAllocInfo.allocationSize = imageMemoryRequirements.size;
    imageAllocInfo.memoryTypeIndex = findMemoryType(
        imageMemoryRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device_, &imageAllocInfo, nullptr, &resource.imageMemory) != VK_SUCCESS ||
        vkBindImageMemory(device_, resource.image, resource.imageMemory, 0) != VK_SUCCESS) {
      cleanup();
      outError = "failed to allocate vector text image memory";
      return false;
    }

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
      cleanup();
      outError = "failed to create vector text image view";
      return false;
    }

    VkDescriptorSetAllocateInfo descriptorAllocInfo{};
    descriptorAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descriptorAllocInfo.descriptorPool = uiVectorTextComputeDescriptorPool_;
    descriptorAllocInfo.descriptorSetCount = 1;
    descriptorAllocInfo.pSetLayouts = &uiVectorTextComputeDescriptorSetLayout_;
    if (vkAllocateDescriptorSets(device_, &descriptorAllocInfo, &computeDescriptorSet) != VK_SUCCESS) {
      cleanup();
      outError = "failed to allocate vector text descriptor set";
      return false;
    }

    VkDescriptorSetAllocateInfo prefixScanDescriptorAllocInfo{};
    prefixScanDescriptorAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    prefixScanDescriptorAllocInfo.descriptorPool = uiVectorTextComputeDescriptorPool_;
    prefixScanDescriptorAllocInfo.descriptorSetCount = 1;
    prefixScanDescriptorAllocInfo.pSetLayouts = &uiVectorTextPrefixScanDescriptorSetLayout_;
    if (vkAllocateDescriptorSets(device_, &prefixScanDescriptorAllocInfo, &prefixScanDescriptorSet) != VK_SUCCESS) {
      cleanup();
      outError = "failed to allocate vector text prefix scan descriptor set";
      return false;
    }

    if (uiVectorTextExclusiveScanDescriptorSetLayout_ != VK_NULL_HANDLE) {
      VkDescriptorSetAllocateInfo excScanDescAllocInfo{};
      excScanDescAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
      excScanDescAllocInfo.descriptorPool = uiVectorTextComputeDescriptorPool_;
      excScanDescAllocInfo.descriptorSetCount = 1;
      excScanDescAllocInfo.pSetLayouts = &uiVectorTextExclusiveScanDescriptorSetLayout_;
      if (vkAllocateDescriptorSets(device_, &excScanDescAllocInfo, &exclusiveScanDescriptorSet) != VK_SUCCESS) {
        cleanup();
        outError = "failed to allocate exclusive scan descriptor set";
        return false;
      }
    }

    auto updateDescriptorSet = [&]() {
      const VkDescriptorBufferInfo bufferInfos[] = {
          {curveBuffer.buffer, 0, curveBuffer.size},
          {curveCountBuffer.buffer, 0, curveCountBuffer.size},
          {curveOffsetBuffer.buffer, 0, curveOffsetBuffer.size},
          {segmentBuffer.buffer, 0, segmentBuffer.size},
          {curveMetaBuffer.buffer, 0, curveMetaBuffer.size},
          {tileCountBuffer.buffer, 0, tileCountBuffer.size},
          {tileOffsetBuffer.buffer, 0, tileOffsetBuffer.size},
          {tileFillPtrBuffer.buffer, 0, tileFillPtrBuffer.size},
          {tileSegmentListBuffer.buffer, 0, tileSegmentListBuffer.size},
          {backdropBuffer.buffer, 0, backdropBuffer.size},
      };
      VkDescriptorImageInfo imageDescriptorInfo{};
      imageDescriptorInfo.imageView = resource.imageView;
      imageDescriptorInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

      std::array<VkWriteDescriptorSet, 11> writes{};
      for (std::uint32_t i = 0U; i < 10U; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = computeDescriptorSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &bufferInfos[i];
      }
      writes[10].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[10].dstSet = computeDescriptorSet;
      writes[10].dstBinding = 10;
      writes[10].descriptorCount = 1;
      writes[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      writes[10].pImageInfo = &imageDescriptorInfo;
      vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    };

    std::function<bool(
      const TempBuffer&,
      TempBuffer&,
      std::uint32_t,
      std::uint32_t&,
      bool&,
      std::uint32_t,
      std::uint32_t)> runPrefixScan;

    runPrefixScan = [&](const TempBuffer& inputCountsBuffer,
              TempBuffer& outputOffsetsBuffer,
              std::uint32_t elementCount,
              std::uint32_t& outTotal,
              bool& outUsedGpuPath,
              std::uint32_t beginTimestampQuery,
              std::uint32_t endTimestampQuery) -> bool {
      outTotal = 0U;
      outUsedGpuPath = false;
      if (elementCount == 0U) {
        return true;
      }

      const bool canUseGpuPath =
          uiVectorTextPrefixScanPipeline_ != VK_NULL_HANDLE &&
          uiVectorTextPrefixScanPipelineLayout_ != VK_NULL_HANDLE &&
          uiVectorTextPrefixScanDescriptorSetLayout_ != VK_NULL_HANDLE &&
          prefixScanDescriptorSet != VK_NULL_HANDLE &&
          elementCount <= 256U;

      if (!canUseGpuPath) {
        // Try multi-workgroup GPU path for elementCount > 256.
        const bool canUseMultiWgPath =
            uiVectorTextExclusiveScanPipeline_ != VK_NULL_HANDLE &&
            uiVectorTextExclusiveScanPipelineLayout_ != VK_NULL_HANDLE &&
            exclusiveScanDescriptorSet != VK_NULL_HANDLE;

        if (canUseMultiWgPath) {
          const std::uint32_t numWorkgroups = (elementCount + 255U) / 256U;

          // Copy input counts into output offsets buffer; exclusive_scan works in-place.
          if (!writeUintBuffer(exclusiveScanParamsBuffer, std::vector<std::uint32_t>{elementCount, 0U})) {
            return false;
          }
          {
            const VkDescriptorBufferInfo countsBuf{outputOffsetsBuffer.buffer, 0, static_cast<VkDeviceSize>(elementCount * sizeof(std::uint32_t))};
            const VkDescriptorBufferInfo blockBuf{exclusiveScanBlockSumsBuffer.buffer, 0, static_cast<VkDeviceSize>(numWorkgroups * sizeof(std::uint32_t))};
            const VkDescriptorBufferInfo paramsBuf{exclusiveScanParamsBuffer.buffer, 0, exclusiveScanParamsBuffer.size};
            const std::array<VkWriteDescriptorSet, 3> excW = {
                VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, exclusiveScanDescriptorSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &countsBuf, nullptr},
                VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, exclusiveScanDescriptorSet, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &blockBuf, nullptr},
                VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, exclusiveScanDescriptorSet, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &paramsBuf, nullptr},
            };
            vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(excW.size()), excW.data(), 0, nullptr);
          }

            const VkDescriptorBufferInfo blockSumsInputInfo{
              exclusiveScanBlockSumsBuffer.buffer,
              0,
              static_cast<VkDeviceSize>(numWorkgroups * sizeof(std::uint32_t)),
          };
            const VkDescriptorBufferInfo blockSumsOutputInfo{
              exclusiveScanBlockSumsBuffer.buffer,
              0,
              static_cast<VkDeviceSize>(numWorkgroups * sizeof(std::uint32_t)),
          };
          const VkDescriptorBufferInfo outputTotalInfo{
              prefixScanTotalBuffer.buffer,
              0,
              prefixScanTotalBuffer.size,
          };
          const std::array<VkWriteDescriptorSet, 3> prefixWrites = {
              VkWriteDescriptorSet{
                  VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                  nullptr,
                  prefixScanDescriptorSet,
                  0,
                  0,
                  1,
                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                  nullptr,
                    &blockSumsInputInfo,
                  nullptr,
              },
              VkWriteDescriptorSet{
                  VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                  nullptr,
                  prefixScanDescriptorSet,
                  1,
                  0,
                  1,
                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                  nullptr,
                    &blockSumsOutputInfo,
                  nullptr,
              },
              VkWriteDescriptorSet{
                  VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                  nullptr,
                  prefixScanDescriptorSet,
                  2,
                  0,
                  1,
                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                  nullptr,
                  &outputTotalInfo,
                  nullptr,
              },
          };
          vkUpdateDescriptorSets(
              device_,
              static_cast<std::uint32_t>(prefixWrites.size()),
              prefixWrites.data(),
              0,
              nullptr);

          if (!submitComputeWork([&](VkCommandBuffer commandBuffer) {
                VkBufferCopy copyRegion{};
                copyRegion.srcOffset = 0;
                copyRegion.dstOffset = 0;
                copyRegion.size = static_cast<VkDeviceSize>(elementCount * sizeof(std::uint32_t));
                vkCmdCopyBuffer(commandBuffer, inputCountsBuffer.buffer, outputOffsetsBuffer.buffer, 1, &copyRegion);

                VkBufferMemoryBarrier transferToCompute{};
                transferToCompute.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                transferToCompute.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                transferToCompute.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                transferToCompute.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                transferToCompute.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                transferToCompute.buffer = outputOffsetsBuffer.buffer;
                transferToCompute.offset = 0;
                transferToCompute.size = copyRegion.size;
                vkCmdPipelineBarrier(
                    commandBuffer,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    0,
                    0,
                    nullptr,
                    1,
                    &transferToCompute,
                    0,
                    nullptr);

                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, uiVectorTextExclusiveScanPipeline_);
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                    uiVectorTextExclusiveScanPipelineLayout_, 0, 1, &exclusiveScanDescriptorSet, 0, nullptr);
                vkCmdDispatch(commandBuffer, numWorkgroups, 1, 1);
              },
              beginTimestampQuery,
              UINT32_MAX)) {
            return false;
          }

          // Pass 1: scan block sums, then add scanned block offsets back to each element.
          if (numWorkgroups > 256U) {
            [[maybe_unused]] bool usedGpuBlockScanPath = false;
            if (!runPrefixScan(
                    exclusiveScanBlockSumsBuffer,
                    exclusiveScanBlockSumsScratchBuffer,
                    numWorkgroups,
                    outTotal,
                    usedGpuBlockScanPath,
                    UINT32_MAX,
                    UINT32_MAX)) {
              return false;
            }

            if (!copyBufferOnGpu(
                    exclusiveScanBlockSumsScratchBuffer,
                    exclusiveScanBlockSumsBuffer,
                    static_cast<VkDeviceSize>(numWorkgroups * sizeof(std::uint32_t)))) {
              return false;
            }

            if (!writeUintBuffer(exclusiveScanParamsBuffer, std::vector<std::uint32_t>{elementCount, 1U})) {
              return false;
            }
            if (!submitComputeWork([&](VkCommandBuffer commandBuffer) {
                  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, uiVectorTextExclusiveScanPipeline_);
                  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                      uiVectorTextExclusiveScanPipelineLayout_, 0, 1, &exclusiveScanDescriptorSet, 0, nullptr);
                  vkCmdDispatch(commandBuffer, numWorkgroups, 1, 1);
                },
                UINT32_MAX,
                endTimestampQuery)) {
              return false;
            }
          } else {
            if (!writeUintBuffer(exclusiveScanParamsBuffer, std::vector<std::uint32_t>{elementCount, 1U})) {
              return false;
            }
            if (!submitComputeWork([&](VkCommandBuffer commandBuffer) {
                  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, uiVectorTextPrefixScanPipeline_);
                  vkCmdBindDescriptorSets(
                      commandBuffer,
                      VK_PIPELINE_BIND_POINT_COMPUTE,
                      uiVectorTextPrefixScanPipelineLayout_,
                      0,
                      1,
                      &prefixScanDescriptorSet,
                      0,
                      nullptr);
                  vkCmdDispatch(commandBuffer, 1, 1, 1);

                  VkMemoryBarrier barrier{};
                  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                  barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                  vkCmdPipelineBarrier(
                      commandBuffer,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      0,
                      1,
                      &barrier,
                      0,
                      nullptr,
                      0,
                      nullptr);

                  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, uiVectorTextExclusiveScanPipeline_);
                  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                      uiVectorTextExclusiveScanPipelineLayout_, 0, 1, &exclusiveScanDescriptorSet, 0, nullptr);
                  vkCmdDispatch(commandBuffer, numWorkgroups, 1, 1);
                },
                UINT32_MAX,
                endTimestampQuery)) {
              return false;
            }

            std::vector<std::uint32_t> totalValues;
            if (!readUintBuffer(prefixScanTotalBuffer, 1U, totalValues)) {
              return false;
            }
            outTotal = totalValues.empty() ? 0U : totalValues.front();
          }

          outUsedGpuPath = true;
          return true;
        }

        // CPU fallback.
        std::vector<std::uint32_t> counts;
        std::vector<std::uint32_t> offsets;
        if (!readUintBuffer(inputCountsBuffer, elementCount, counts)) {
          return false;
        }
        outTotal = computeExclusiveScanTotal(counts, offsets);
        if (!writeUintBuffer(outputOffsetsBuffer, offsets)) {
          return false;
        }
        return true;
      }

      const VkDescriptorBufferInfo inputCountsInfo{
          inputCountsBuffer.buffer,
          0,
          inputCountsBuffer.size,
      };
      const VkDescriptorBufferInfo outputOffsetsInfo{
          outputOffsetsBuffer.buffer,
          0,
          outputOffsetsBuffer.size,
      };
      const VkDescriptorBufferInfo outputTotalInfo{
          prefixScanTotalBuffer.buffer,
          0,
          prefixScanTotalBuffer.size,
      };

      const std::array<VkWriteDescriptorSet, 3> writes = {
          VkWriteDescriptorSet{
              VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
              nullptr,
              prefixScanDescriptorSet,
              0,
              0,
              1,
              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
              nullptr,
              &inputCountsInfo,
              nullptr,
          },
          VkWriteDescriptorSet{
              VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
              nullptr,
              prefixScanDescriptorSet,
              1,
              0,
              1,
              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
              nullptr,
              &outputOffsetsInfo,
              nullptr,
          },
          VkWriteDescriptorSet{
              VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
              nullptr,
              prefixScanDescriptorSet,
              2,
              0,
              1,
              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
              nullptr,
              &outputTotalInfo,
              nullptr,
          },
      };
      vkUpdateDescriptorSets(
          device_,
          static_cast<std::uint32_t>(writes.size()),
          writes.data(),
          0,
          nullptr);

      if (!submitComputeWork([&](VkCommandBuffer commandBuffer) {
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, uiVectorTextPrefixScanPipeline_);
            vkCmdBindDescriptorSets(
                commandBuffer,
                VK_PIPELINE_BIND_POINT_COMPUTE,
                uiVectorTextPrefixScanPipelineLayout_,
                0,
                1,
                &prefixScanDescriptorSet,
                0,
                nullptr);
            vkCmdDispatch(commandBuffer, 1, 1, 1);
          }, beginTimestampQuery, endTimestampQuery)) {
        return false;
      }

      std::vector<std::uint32_t> totalValues;
      if (!readUintBuffer(prefixScanTotalBuffer, 1U, totalValues)) {
        return false;
      }

      outTotal = totalValues.empty() ? 0U : totalValues.front();
      outUsedGpuPath = true;
      return true;
    };

    TempBuffer initialDummy{};
    if (!createHostVisibleBuffer(sizeof(std::uint32_t), initialDummy, nullptr)) {
      cleanup();
      return false;
    }
    segmentBuffer = initialDummy;
    initialDummy = TempBuffer{};
    if (!createHostVisibleBuffer(sizeof(std::uint32_t), curveMetaBuffer, nullptr) ||
        !createHostVisibleBuffer(sizeof(std::uint32_t), tileSegmentListBuffer, nullptr)) {
      cleanup();
      return false;
    }
    updateDescriptorSet();

    constexpr std::uint32_t kTsFlattenCountBegin = 0;
    constexpr std::uint32_t kTsFlattenCountEnd = 1;
    constexpr std::uint32_t kTsCurveScanBegin = 2;
    constexpr std::uint32_t kTsCurveScanEnd = 3;
    constexpr std::uint32_t kTsFlattenEmitBinCountBegin = 4;
    constexpr std::uint32_t kTsFlattenEmitBinCountEnd = 5;
    constexpr std::uint32_t kTsTileScanBegin = 6;
    constexpr std::uint32_t kTsTileScanEnd = 7;
    constexpr std::uint32_t kTsBinEmitFineBegin = 8;
    constexpr std::uint32_t kTsBinEmitFineEnd = 9;

    UiVectorTextPushConstants pushConstants{};
    pushConstants.curveCount = curveCount;
    pushConstants.imageWidth = imageWidth;
    pushConstants.imageHeight = imageHeight;
    pushConstants.tilesWide = tilesWide;
    pushConstants.tilesHigh = tilesHigh;
    pushConstants.flatnessThresholdPx = 0.25F;
    pushConstants.color = batch.color;

    if (!submitComputeWork([&](VkCommandBuffer commandBuffer) {
          vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, uiVectorTextFlattenCountPipeline_);
          vkCmdBindDescriptorSets(
              commandBuffer,
              VK_PIPELINE_BIND_POINT_COMPUTE,
              uiVectorTextComputePipelineLayout_,
              0,
              1,
              &computeDescriptorSet,
              0,
              nullptr);
          vkCmdPushConstants(
              commandBuffer,
              uiVectorTextComputePipelineLayout_,
              VK_SHADER_STAGE_COMPUTE_BIT,
              0,
              sizeof(UiVectorTextPushConstants),
              &pushConstants);
          vkCmdDispatch(commandBuffer, std::max(1U, (curveCount + 255U) / 256U), 1, 1);
        },
        kTsFlattenCountBegin,
        kTsFlattenCountEnd)) {
      cleanup();
      return false;
    }

    std::uint32_t segmentCount = 0U;
    [[maybe_unused]] bool usedGpuCurveScan = false;
    if (!runPrefixScan(
            curveCountBuffer,
            curveOffsetBuffer,
            curveCount,
            segmentCount,
            usedGpuCurveScan,
            kTsCurveScanBegin,
            kTsCurveScanEnd)) {
      cleanup();
      return false;
    }
    if (segmentCount == 0U) {
      outError = "vector text flatten produced zero segments";
      cleanup();
      return false;
    }
    pushConstants.segmentCount = segmentCount;

    destroyTempBuffer(segmentBuffer);
    destroyTempBuffer(curveMetaBuffer);
    if (!createHostVisibleBuffer(static_cast<VkDeviceSize>(segmentCount * sizeof(GpuLineSegment)), segmentBuffer, nullptr) ||
        !createHostVisibleBuffer(static_cast<VkDeviceSize>(curveCount * sizeof(GpuCurveMeta)), curveMetaBuffer, nullptr) ||
        !createHostVisibleBuffer(static_cast<VkDeviceSize>(tileCount * sizeof(std::int32_t)), backdropBuffer, nullptr)) {
      cleanup();
      return false;
    }
    updateDescriptorSet();

    if (!submitComputeWork([&](VkCommandBuffer commandBuffer) {
          vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, uiVectorTextFlattenEmitPipeline_);
          vkCmdBindDescriptorSets(
              commandBuffer,
              VK_PIPELINE_BIND_POINT_COMPUTE,
              uiVectorTextComputePipelineLayout_,
              0,
              1,
              &computeDescriptorSet,
              0,
              nullptr);
          vkCmdPushConstants(
              commandBuffer,
              uiVectorTextComputePipelineLayout_,
              VK_SHADER_STAGE_COMPUTE_BIT,
              0,
              sizeof(UiVectorTextPushConstants),
              &pushConstants);

          VkMemoryBarrier barrier{};
          barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
          barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
          barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

          vkCmdDispatch(commandBuffer, std::max(1U, (curveCount + 255U) / 256U), 1, 1);
          vkCmdPipelineBarrier(
              commandBuffer,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
              0,
              1,
              &barrier,
              0,
              nullptr,
              0,
              nullptr);

          vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, uiVectorTextBinCountPipeline_);
          vkCmdPushConstants(
              commandBuffer,
              uiVectorTextComputePipelineLayout_,
              VK_SHADER_STAGE_COMPUTE_BIT,
              0,
              sizeof(UiVectorTextPushConstants),
              &pushConstants);
          vkCmdDispatch(commandBuffer, std::max(1U, (segmentCount + 255U) / 256U), 1, 1);
        },
        kTsFlattenEmitBinCountBegin,
        kTsFlattenEmitBinCountEnd)) {
      cleanup();
      return false;
    }

    std::uint32_t totalTileTouches = 0U;
    [[maybe_unused]] bool usedGpuTileScan = false;
    if (!runPrefixScan(
            tileCountBuffer,
            tileOffsetBuffer,
            tileCount,
            totalTileTouches,
            usedGpuTileScan,
            kTsTileScanBegin,
            kTsTileScanEnd)) {
      cleanup();
      return false;
    }
    if (!copyBufferOnGpu(tileOffsetBuffer, tileFillPtrBuffer, static_cast<VkDeviceSize>(tileCount * sizeof(std::uint32_t)))) {
      cleanup();
      return false;
    }

    destroyTempBuffer(tileSegmentListBuffer);
    if (!createHostVisibleBuffer(static_cast<VkDeviceSize>(totalTileTouches * sizeof(std::uint32_t)), tileSegmentListBuffer, nullptr)) {
      cleanup();
      return false;
    }
    updateDescriptorSet();

    if (!submitComputeWork([&](VkCommandBuffer commandBuffer) {
          VkImageMemoryBarrier imageToGeneral{};
          imageToGeneral.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
          imageToGeneral.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
          imageToGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
          imageToGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          imageToGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          imageToGeneral.image = resource.image;
          imageToGeneral.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
          imageToGeneral.subresourceRange.baseMipLevel = 0;
          imageToGeneral.subresourceRange.levelCount = 1;
          imageToGeneral.subresourceRange.baseArrayLayer = 0;
          imageToGeneral.subresourceRange.layerCount = 1;
          imageToGeneral.srcAccessMask = 0;
          imageToGeneral.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
          vkCmdPipelineBarrier(
              commandBuffer,
              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
              0,
              0,
              nullptr,
              0,
              nullptr,
              1,
              &imageToGeneral);

          VkClearColorValue clearColor{};
          VkImageSubresourceRange clearRange{};
          clearRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
          clearRange.baseMipLevel = 0;
          clearRange.levelCount = 1;
          clearRange.baseArrayLayer = 0;
          clearRange.layerCount = 1;
          vkCmdClearColorImage(commandBuffer, resource.image, VK_IMAGE_LAYOUT_GENERAL, &clearColor, 1, &clearRange);

          vkCmdBindDescriptorSets(
              commandBuffer,
              VK_PIPELINE_BIND_POINT_COMPUTE,
              uiVectorTextComputePipelineLayout_,
              0,
              1,
              &computeDescriptorSet,
              0,
              nullptr);

          VkMemoryBarrier barrier{};
          barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
          barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
          barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

          vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, uiVectorTextBinEmitPipeline_);
          vkCmdPushConstants(
              commandBuffer,
              uiVectorTextComputePipelineLayout_,
              VK_SHADER_STAGE_COMPUTE_BIT,
              0,
              sizeof(UiVectorTextPushConstants),
              &pushConstants);
          vkCmdDispatch(commandBuffer, std::max(1U, (segmentCount + 255U) / 256U), 1, 1);

          vkCmdPipelineBarrier(
              commandBuffer,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
              0,
              1,
              &barrier,
              0,
              nullptr,
              0,
              nullptr);

          vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, uiVectorTextFinePipeline_);
          vkCmdPushConstants(
              commandBuffer,
              uiVectorTextComputePipelineLayout_,
              VK_SHADER_STAGE_COMPUTE_BIT,
              0,
              sizeof(UiVectorTextPushConstants),
              &pushConstants);
          vkCmdDispatch(commandBuffer, std::max(1U, (imageWidth + 7U) / 8U), std::max(1U, (imageHeight + 7U) / 8U), 1);

          VkImageMemoryBarrier imageToSampled{};
          imageToSampled.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
          imageToSampled.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
          imageToSampled.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
          imageToSampled.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          imageToSampled.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          imageToSampled.image = resource.image;
          imageToSampled.subresourceRange = imageToGeneral.subresourceRange;
          imageToSampled.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
          imageToSampled.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(
              commandBuffer,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
              0,
              0,
              nullptr,
              0,
              nullptr,
                1,
                &imageToSampled);
              },
              kTsBinEmitFineBegin,
              kTsBinEmitFineEnd)) {
      cleanup();
      return false;
    }

    VkDescriptorSetAllocateInfo graphicsDescriptorAllocInfo{};
    graphicsDescriptorAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    graphicsDescriptorAllocInfo.descriptorPool = uiDescriptorPool_;
    graphicsDescriptorAllocInfo.descriptorSetCount = 1;
    graphicsDescriptorAllocInfo.pSetLayouts = &uiDescriptorSetLayout_;
    if (vkAllocateDescriptorSets(device_, &graphicsDescriptorAllocInfo, &resource.descriptorSet) != VK_SUCCESS) {
      cleanup();
      outError = "failed to allocate vector text graphics descriptor set";
      return false;
    }

    VkDescriptorImageInfo sampledImageInfo{};
    sampledImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    sampledImageInfo.imageView = resource.imageView;
    sampledImageInfo.sampler = uiTextureSampler_;

    VkWriteDescriptorSet graphicsWrite{};
    graphicsWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    graphicsWrite.dstSet = resource.descriptorSet;
    graphicsWrite.dstBinding = 0;
    graphicsWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    graphicsWrite.descriptorCount = 1;
    graphicsWrite.pImageInfo = &sampledImageInfo;
    vkUpdateDescriptorSets(device_, 1, &graphicsWrite, 0, nullptr);

    if (vectorTextTimestampQueryPool != VK_NULL_HANDLE) {
      std::array<std::uint64_t, kVectorTextTimestampCount> timestamps{};
      const VkResult queryResult = vkGetQueryPoolResults(
          device_,
          vectorTextTimestampQueryPool,
          0,
          kVectorTextTimestampCount,
          sizeof(timestamps),
          timestamps.data(),
          sizeof(std::uint64_t),
          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
      if (queryResult == VK_SUCCESS) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physicalDevice_, &properties);
        const double nsToMs = static_cast<double>(properties.limits.timestampPeriod) / 1'000'000.0;
        const auto deltaMs = [&](std::uint32_t begin, std::uint32_t end) -> double {
          if (end <= begin) {
            return 0.0;
          }
          const std::uint64_t beginTicks = timestamps[begin];
          const std::uint64_t endTicks = timestamps[end];
          if (endTicks < beginTicks) {
            return 0.0;
          }
          return static_cast<double>(endTicks - beginTicks) * nsToMs;
        };

        VOLT_LOG_TRACE_CAT(
            volt::core::logging::Category::kRender,
            "Vector text GPU timings (ms): flatten_count=",
            deltaMs(kTsFlattenCountBegin, kTsFlattenCountEnd),
            " curve_scan=",
            deltaMs(kTsCurveScanBegin, kTsCurveScanEnd),
            " flatten_emit+bin_count=",
            deltaMs(kTsFlattenEmitBinCountBegin, kTsFlattenEmitBinCountEnd),
            " tile_scan=",
            deltaMs(kTsTileScanBegin, kTsTileScanEnd),
            " bin_emit+fine=",
            deltaMs(kTsBinEmitFineBegin, kTsBinEmitFineEnd));

          latestVectorTextGpuTimings_.valid = true;
          latestVectorTextGpuTimings_.flattenCountMs = static_cast<std::int32_t>(
            std::lround(deltaMs(kTsFlattenCountBegin, kTsFlattenCountEnd)));
          latestVectorTextGpuTimings_.curveScanMs = static_cast<std::int32_t>(
            std::lround(deltaMs(kTsCurveScanBegin, kTsCurveScanEnd)));
          latestVectorTextGpuTimings_.flattenEmitBinCountMs = static_cast<std::int32_t>(
            std::lround(deltaMs(kTsFlattenEmitBinCountBegin, kTsFlattenEmitBinCountEnd)));
          latestVectorTextGpuTimings_.tileScanMs = static_cast<std::int32_t>(
            std::lround(deltaMs(kTsTileScanBegin, kTsTileScanEnd)));
          latestVectorTextGpuTimings_.binEmitFineMs = static_cast<std::int32_t>(
            std::lround(deltaMs(kTsBinEmitFineBegin, kTsBinEmitFineEnd)));
      }
    }

    uiTextureResources_[batch.textureKey] = resource;
    uiVectorTextTransientKeys_.insert(batch.textureKey);

    if (computeDescriptorSet != VK_NULL_HANDLE) {
      vkFreeDescriptorSets(device_, uiVectorTextComputeDescriptorPool_, 1, &computeDescriptorSet);
      computeDescriptorSet = VK_NULL_HANDLE;
    }
    if (prefixScanDescriptorSet != VK_NULL_HANDLE) {
      vkFreeDescriptorSets(device_, uiVectorTextComputeDescriptorPool_, 1, &prefixScanDescriptorSet);
      prefixScanDescriptorSet = VK_NULL_HANDLE;
    }
    if (exclusiveScanDescriptorSet != VK_NULL_HANDLE) {
      vkFreeDescriptorSets(device_, uiVectorTextComputeDescriptorPool_, 1, &exclusiveScanDescriptorSet);
      exclusiveScanDescriptorSet = VK_NULL_HANDLE;
    }
    destroyTempBuffer(prefixScanTotalBuffer);
    destroyTempBuffer(exclusiveScanParamsBuffer);
    destroyTempBuffer(exclusiveScanBlockSumsBuffer);
    destroyTempBuffer(exclusiveScanBlockSumsScratchBuffer);
    destroyTempBuffer(backdropBuffer);
    destroyTempBuffer(tileSegmentListBuffer);
    destroyTempBuffer(tileFillPtrBuffer);
    destroyTempBuffer(tileOffsetBuffer);
    destroyTempBuffer(tileCountBuffer);
    destroyTempBuffer(curveMetaBuffer);
    destroyTempBuffer(segmentBuffer);
    destroyTempBuffer(curveOffsetBuffer);
    destroyTempBuffer(curveCountBuffer);
    destroyTempBuffer(curveBuffer);
    if (vectorTextTimestampQueryPool != VK_NULL_HANDLE) {
      vkDestroyQueryPool(device_, vectorTextTimestampQueryPool, nullptr);
      vectorTextTimestampQueryPool = VK_NULL_HANDLE;
    }
    if (reusableComputeFence != VK_NULL_HANDLE) {
      vkDestroyFence(device_, reusableComputeFence, nullptr);
      reusableComputeFence = VK_NULL_HANDLE;
    }
    if (reusableComputeCommandBuffer != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(device_, scaffoldCommandPool_, 1, &reusableComputeCommandBuffer);
      reusableComputeCommandBuffer = VK_NULL_HANDLE;
    }
    return true;
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
    attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
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

    const auto vertexShader = readBinaryFile("assets/shaders/ui.vert.spv");
    const auto fragmentShader = readBinaryFile("assets/shaders/scaffold.frag.spv");
    if (!vertexShader.has_value() || !fragmentShader.has_value()) {
      VOLT_LOG_ERROR_CAT(
          volt::core::logging::Category::kRender,
          "Missing UI shaders at assets/shaders/ui.vert.spv or scaffold.frag.spv");
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
    attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
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
    colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
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

  void VulkanRenderer::createUiOpaqueGraphicsPipeline()
  {
    if (scaffoldPipelineLayout_ == VK_NULL_HANDLE || scaffoldRenderPass_ == VK_NULL_HANDLE) {
      return;
    }

    const auto vertexShader = readBinaryFile("assets/shaders/ui.vert.spv");
    const auto fragmentShader = readBinaryFile("assets/shaders/scaffold.frag.spv");
    if (!vertexShader.has_value() || !fragmentShader.has_value()) {
      VOLT_LOG_ERROR_CAT(
          volt::core::logging::Category::kRender,
          "Missing opaque UI shaders at assets/shaders/ui.vert.spv or scaffold.frag.spv");
      uiOpaqueGraphicsPipeline_ = VK_NULL_HANDLE;
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
    attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
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
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
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
        &uiOpaqueGraphicsPipeline_);

    vkDestroyShaderModule(device_, fragShaderModule, nullptr);
    vkDestroyShaderModule(device_, vertShaderModule, nullptr);

    if (pipelineResult != VK_SUCCESS) {
      throw std::runtime_error("Failed to create opaque UI graphics pipeline");
    }
  }

  void VulkanRenderer::createUiTextSdfGraphicsPipeline()
  {
    if (scaffoldPipelineLayout_ == VK_NULL_HANDLE || scaffoldRenderPass_ == VK_NULL_HANDLE) {
      return;
    }

    const auto vertexShader = readBinaryFile("assets/shaders/ui.vert.spv");
    const auto fragmentShader = readBinaryFile("assets/shaders/ui_text_sdf.frag.spv");
    if (!vertexShader.has_value() || !fragmentShader.has_value()) {
      VOLT_LOG_ERROR_CAT(
          volt::core::logging::Category::kRender,
          "Missing UI SDF text shaders at assets/shaders/ui.vert.spv or ui_text_sdf.frag.spv");
      uiTextSdfGraphicsPipeline_ = VK_NULL_HANDLE;
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
    attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
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
        &uiTextSdfGraphicsPipeline_);

    vkDestroyShaderModule(device_, fragShaderModule, nullptr);
    vkDestroyShaderModule(device_, vertShaderModule, nullptr);

    if (pipelineResult != VK_SUCCESS) {
      throw std::runtime_error("Failed to create UI SDF text graphics pipeline");
    }
  }

  void VulkanRenderer::createUiOffscreenGraphicsPipeline()
  {
    if (scaffoldPipelineLayout_ == VK_NULL_HANDLE || uiOffscreenRenderPass_ == VK_NULL_HANDLE) {
      return;
    }

    const auto vertexShader = readBinaryFile("assets/shaders/ui.vert.spv");
    const auto fragmentShader = readBinaryFile("assets/shaders/scaffold.frag.spv");
    if (!vertexShader.has_value() || !fragmentShader.has_value()) {
      VOLT_LOG_ERROR_CAT(
          volt::core::logging::Category::kRender,
          "Missing offscreen UI shaders at assets/shaders/ui.vert.spv or scaffold.frag.spv");
      uiOffscreenGraphicsPipeline_ = VK_NULL_HANDLE;
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

    const std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages = {vertShaderStageInfo, fragShaderStageInfo};

    VkVertexInputBindingDescription bindingDescription{};
    bindingDescription.binding = 0;
    bindingDescription.stride = static_cast<uint32_t>(sizeof(volt::ui::UiVertex));
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 3> attributeDescriptions{};
    attributeDescriptions[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(volt::ui::UiVertex, x))};
    attributeDescriptions[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(volt::ui::UiVertex, u))};
    attributeDescriptions[2] = {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, static_cast<uint32_t>(offsetof(volt::ui::UiVertex, color))};

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
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
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0F;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_TRUE;
    colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    constexpr std::array<VkDynamicState, 2> dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
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
    pipelineInfo.renderPass = uiOffscreenRenderPass_;
    pipelineInfo.subpass = 0;

    const VkResult pipelineResult = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &uiOffscreenGraphicsPipeline_);

    vkDestroyShaderModule(device_, fragShaderModule, nullptr);
    vkDestroyShaderModule(device_, vertShaderModule, nullptr);

    if (pipelineResult != VK_SUCCESS) {
      throw std::runtime_error("Failed to create offscreen UI graphics pipeline");
    }
  }

  void VulkanRenderer::createUiOffscreenTextSdfGraphicsPipeline()
  {
    if (scaffoldPipelineLayout_ == VK_NULL_HANDLE || uiOffscreenRenderPass_ == VK_NULL_HANDLE) {
      return;
    }

    const auto vertexShader = readBinaryFile("assets/shaders/ui.vert.spv");
    const auto fragmentShader = readBinaryFile("assets/shaders/ui_text_sdf.frag.spv");
    if (!vertexShader.has_value() || !fragmentShader.has_value()) {
      VOLT_LOG_ERROR_CAT(
          volt::core::logging::Category::kRender,
          "Missing offscreen UI SDF text shaders at assets/shaders/ui.vert.spv or ui_text_sdf.frag.spv");
      uiOffscreenTextSdfGraphicsPipeline_ = VK_NULL_HANDLE;
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

    const std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages = {vertShaderStageInfo, fragShaderStageInfo};

    VkVertexInputBindingDescription bindingDescription{};
    bindingDescription.binding = 0;
    bindingDescription.stride = static_cast<uint32_t>(sizeof(volt::ui::UiVertex));
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 3> attributeDescriptions{};
    attributeDescriptions[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(volt::ui::UiVertex, x))};
    attributeDescriptions[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(volt::ui::UiVertex, u))};
    attributeDescriptions[2] = {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, static_cast<uint32_t>(offsetof(volt::ui::UiVertex, color))};

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
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
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0F;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_TRUE;
    colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    constexpr std::array<VkDynamicState, 2> dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
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
    pipelineInfo.renderPass = uiOffscreenRenderPass_;
    pipelineInfo.subpass = 0;

    const VkResult pipelineResult = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &uiOffscreenTextSdfGraphicsPipeline_);

    vkDestroyShaderModule(device_, fragShaderModule, nullptr);
    vkDestroyShaderModule(device_, vertShaderModule, nullptr);

    if (pipelineResult != VK_SUCCESS) {
      throw std::runtime_error("Failed to create offscreen UI SDF text graphics pipeline");
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

  void VulkanRenderer::cleanupUiOffscreenRenderPass()
  {
    if (uiOffscreenRenderPass_ != VK_NULL_HANDLE) {
      vkDestroyRenderPass(device_, uiOffscreenRenderPass_, nullptr);
      uiOffscreenRenderPass_ = VK_NULL_HANDLE;
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

  void VulkanRenderer::cleanupUiOpaqueGraphicsPipeline()
  {
    if (uiOpaqueGraphicsPipeline_ != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, uiOpaqueGraphicsPipeline_, nullptr);
      uiOpaqueGraphicsPipeline_ = VK_NULL_HANDLE;
    }
  }

  void VulkanRenderer::cleanupUiTextSdfGraphicsPipeline()
  {
    if (uiTextSdfGraphicsPipeline_ != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, uiTextSdfGraphicsPipeline_, nullptr);
      uiTextSdfGraphicsPipeline_ = VK_NULL_HANDLE;
    }
  }

  void VulkanRenderer::cleanupUiOffscreenGraphicsPipeline()
  {
    if (uiOffscreenGraphicsPipeline_ != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, uiOffscreenGraphicsPipeline_, nullptr);
      uiOffscreenGraphicsPipeline_ = VK_NULL_HANDLE;
    }
  }

  void VulkanRenderer::cleanupUiOffscreenTextSdfGraphicsPipeline()
  {
    if (uiOffscreenTextSdfGraphicsPipeline_ != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, uiOffscreenTextSdfGraphicsPipeline_, nullptr);
      uiOffscreenTextSdfGraphicsPipeline_ = VK_NULL_HANDLE;
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

    cleanupUiVectorTextTransientResources();

    for (auto it = uiTextureResources_.begin(); it != uiTextureResources_.end();) {
      const std::string key = it->first;
      ++it;
      destroyUiTextureResource(key, false);
    }
    for (std::uint32_t frameSlot = 0; frameSlot < retiredUiTextureResources_.size(); ++frameSlot) {
      releaseRetiredUiTextureResources(frameSlot);
      releaseRetiredBufferResources(frameSlot);
    }
    unresolvedTextureKeysLogged_.clear();

    cleanupUiFontComputeResources();
  cleanupUiVectorTextComputeResources();

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

  void VulkanRenderer::cleanupUiFontComputeResources()
  {
    if (uiFontComputePipeline_ != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, uiFontComputePipeline_, nullptr);
      uiFontComputePipeline_ = VK_NULL_HANDLE;
    }

    if (uiFontComputeDescriptorPool_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorPool(device_, uiFontComputeDescriptorPool_, nullptr);
      uiFontComputeDescriptorPool_ = VK_NULL_HANDLE;
    }

    if (uiFontComputePipelineLayout_ != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(device_, uiFontComputePipelineLayout_, nullptr);
      uiFontComputePipelineLayout_ = VK_NULL_HANDLE;
    }

    if (uiFontComputeDescriptorSetLayout_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorSetLayout(device_, uiFontComputeDescriptorSetLayout_, nullptr);
      uiFontComputeDescriptorSetLayout_ = VK_NULL_HANDLE;
    }
  }

  void VulkanRenderer::cleanupUiVectorTextComputeResources()
  {
    if (uiVectorTextFinePipeline_ != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, uiVectorTextFinePipeline_, nullptr);
      uiVectorTextFinePipeline_ = VK_NULL_HANDLE;
    }
    if (uiVectorTextPrefixScanPipeline_ != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, uiVectorTextPrefixScanPipeline_, nullptr);
      uiVectorTextPrefixScanPipeline_ = VK_NULL_HANDLE;
    }
    if (uiVectorTextBinEmitPipeline_ != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, uiVectorTextBinEmitPipeline_, nullptr);
      uiVectorTextBinEmitPipeline_ = VK_NULL_HANDLE;
    }
    if (uiVectorTextBinCountPipeline_ != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, uiVectorTextBinCountPipeline_, nullptr);
      uiVectorTextBinCountPipeline_ = VK_NULL_HANDLE;
    }
    if (uiVectorTextFlattenEmitPipeline_ != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, uiVectorTextFlattenEmitPipeline_, nullptr);
      uiVectorTextFlattenEmitPipeline_ = VK_NULL_HANDLE;
    }
    if (uiVectorTextFlattenCountPipeline_ != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, uiVectorTextFlattenCountPipeline_, nullptr);
      uiVectorTextFlattenCountPipeline_ = VK_NULL_HANDLE;
    }
    if (uiVectorTextComputeDescriptorPool_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorPool(device_, uiVectorTextComputeDescriptorPool_, nullptr);
      uiVectorTextComputeDescriptorPool_ = VK_NULL_HANDLE;
    }
    if (uiVectorTextComputePipelineLayout_ != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(device_, uiVectorTextComputePipelineLayout_, nullptr);
      uiVectorTextComputePipelineLayout_ = VK_NULL_HANDLE;
    }
    if (uiVectorTextComputeDescriptorSetLayout_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorSetLayout(device_, uiVectorTextComputeDescriptorSetLayout_, nullptr);
      uiVectorTextComputeDescriptorSetLayout_ = VK_NULL_HANDLE;
    }
    if (uiVectorTextPrefixScanPipelineLayout_ != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(device_, uiVectorTextPrefixScanPipelineLayout_, nullptr);
      uiVectorTextPrefixScanPipelineLayout_ = VK_NULL_HANDLE;
    }
    if (uiVectorTextPrefixScanDescriptorSetLayout_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorSetLayout(device_, uiVectorTextPrefixScanDescriptorSetLayout_, nullptr);
      uiVectorTextPrefixScanDescriptorSetLayout_ = VK_NULL_HANDLE;
    }
    if (uiVectorTextExclusiveScanPipeline_ != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, uiVectorTextExclusiveScanPipeline_, nullptr);
      uiVectorTextExclusiveScanPipeline_ = VK_NULL_HANDLE;
    }
    if (uiVectorTextExclusiveScanPipelineLayout_ != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(device_, uiVectorTextExclusiveScanPipelineLayout_, nullptr);
      uiVectorTextExclusiveScanPipelineLayout_ = VK_NULL_HANDLE;
    }
    if (uiVectorTextExclusiveScanDescriptorSetLayout_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorSetLayout(device_, uiVectorTextExclusiveScanDescriptorSetLayout_, nullptr);
      uiVectorTextExclusiveScanDescriptorSetLayout_ = VK_NULL_HANDLE;
    }
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

    destroyUiScaffoldBuffers(true);
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

    if (vkMapMemory(device_, uiVertexBufferMemory_, 0, static_cast<VkDeviceSize>(vertexBytes), 0, &uiVertexBufferMapped_) != VK_SUCCESS) {
      throw std::runtime_error("Failed to persistently map UI scaffold vertex memory");
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

    if (vkMapMemory(device_, uiIndexBufferMemory_, 0, static_cast<VkDeviceSize>(indexBytes), 0, &uiIndexBufferMapped_) != VK_SUCCESS) {
      throw std::runtime_error("Failed to persistently map UI scaffold index memory");
    }

    uiVertexBufferCapacityBytes_ = vertexBytes;
    uiIndexBufferCapacityBytes_ = indexBytes;
  }

  void VulkanRenderer::destroyUiScaffoldBuffers(bool deferDestruction)
  {
    if (uiVertexBufferMapped_ != nullptr && uiVertexBufferMemory_ != VK_NULL_HANDLE) {
      vkUnmapMemory(device_, uiVertexBufferMemory_);
      uiVertexBufferMapped_ = nullptr;
    }

    if (deferDestruction) {
      retireBufferResource(uiVertexBuffer_, uiVertexBufferMemory_, "ui-scaffold-vertex-buffer");
    } else {
      if (uiVertexBuffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, uiVertexBuffer_, nullptr);
        uiVertexBuffer_ = VK_NULL_HANDLE;
      }

      if (uiVertexBufferMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, uiVertexBufferMemory_, nullptr);
        uiVertexBufferMemory_ = VK_NULL_HANDLE;
      }
    }

    if (uiIndexBufferMapped_ != nullptr && uiIndexBufferMemory_ != VK_NULL_HANDLE) {
      vkUnmapMemory(device_, uiIndexBufferMemory_);
      uiIndexBufferMapped_ = nullptr;
    }

    if (deferDestruction) {
      retireBufferResource(uiIndexBuffer_, uiIndexBufferMemory_, "ui-scaffold-index-buffer");
    } else {
      if (uiIndexBuffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, uiIndexBuffer_, nullptr);
        uiIndexBuffer_ = VK_NULL_HANDLE;
      }

      if (uiIndexBufferMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, uiIndexBufferMemory_, nullptr);
        uiIndexBufferMemory_ = VK_NULL_HANDLE;
      }
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

    if (uiVertexBufferMapped_ == nullptr || uiIndexBufferMapped_ == nullptr) {
      throw std::runtime_error("UI scaffold buffers are not persistently mapped");
    }

    std::memcpy(uiVertexBufferMapped_, meshData.vertices.data(), vertexBytes);
    std::memcpy(uiIndexBufferMapped_, meshData.indices.data(), indexBytes);
  }

  void VulkanRenderer::recordUiMeshDrawsForExtent(
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
      bool updateFrameStats)
  {
    if (uiVertexBuffer_ == VK_NULL_HANDLE || uiIndexBuffer_ == VK_NULL_HANDLE || scaffoldPipelineLayout_ == VK_NULL_HANDLE) {
      return;
    }

    VkDeviceSize vertexOffset = 0;
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &uiVertexBuffer_, &vertexOffset);
    vkCmdBindIndexBuffer(commandBuffer, uiIndexBuffer_, 0, VK_INDEX_TYPE_UINT32);

    VkViewport viewport{};
    viewport.x = 0.0F;
    viewport.y = 0.0F;
    viewport.width = static_cast<float>(targetWidth);
    viewport.height = static_cast<float>(targetHeight);
    viewport.minDepth = 0.0F;
    viewport.maxDepth = 1.0F;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    const volt::math::Mat4f uiProjection =
        volt::math::orthographicRH(0.0F, std::max(1.0F, uiWidth), 0.0F, std::max(1.0F, uiHeight), 0.0F, 1.0F, volt::math::ClipSpaceZ::kZeroToOne);

    std::unordered_map<std::string, VkDescriptorSet> resolvedDescriptorSets;
    resolvedDescriptorSets.reserve(meshData.batches.size());

    std::unordered_map<std::string, int> textureBatchCounts;
    std::unordered_map<std::string, int> textureDrawCounts;
    std::unordered_map<std::string, int> clipBatchCounts;
    std::unordered_map<std::string, int> clipDrawCounts;
    std::unordered_set<std::string> uniqueTextures;
    std::unordered_set<std::string> drawnTextures;
    std::unordered_set<std::string> uniqueClipRects;
    std::unordered_set<std::string> drawnClipRects;

    const auto rectKey = [](const volt::ui::Rect& rect) {
      std::ostringstream stream;
      stream << '[' << rect.x << ',' << rect.y << ',' << rect.width << ',' << rect.height << ']';
      return stream.str();
    };
    const auto topCountsString = [](const std::unordered_map<std::string, int>& counts, std::size_t limit) {
      std::vector<std::pair<std::string, int>> ordered(counts.begin(), counts.end());
      std::sort(
          ordered.begin(),
          ordered.end(),
          [](const auto& left, const auto& right) {
            if (left.second != right.second) {
              return left.second > right.second;
            }
            return left.first < right.first;
          });

      std::ostringstream stream;
      const std::size_t count = std::min(limit, ordered.size());
      for (std::size_t i = 0; i < count; ++i) {
        if (i > 0U) {
          stream << ';';
        }
        stream << ordered[i].first << ':' << ordered[i].second;
      }
      return count == 0U ? std::string("none") : stream.str();
    };

    VkPipeline lastBoundPipeline = VK_NULL_HANDLE;
    VkDescriptorSet lastBoundDescriptor = VK_NULL_HANDLE;
    int drawCallCount = 0;
    double drawCallTotalMs = 0.0;
    double drawCallMaxMs = 0.0;
    std::int32_t drawCallMaxBatchIndex = -1;
    std::string drawCallMaxTextureKey;
    double descriptorResolveTotalMs = 0.0;
    double descriptorResolveMaxMs = 0.0;
    std::string descriptorResolveMaxTextureKey;
    int missingPipelineCount = 0;
    int offscreenBatchCount = 0;
    int zeroScissorBatchCount = 0;
    for (std::size_t batchIdx = 0; batchIdx < meshData.batches.size(); ++batchIdx) {
      const volt::ui::UiMeshBatch& batch = meshData.batches[batchIdx];
      const std::string textureKey = batch.textureKey.empty() ? std::string("__white") : batch.textureKey;
      const std::string clipKey = rectKey(batch.clipRect);
      uniqueTextures.insert(textureKey);
      uniqueClipRects.insert(clipKey);
      ++textureBatchCounts[textureKey];
      ++clipBatchCounts[clipKey];

      auto drawStart = std::chrono::steady_clock::now();
      const bool wantsSdfPipeline = batch.sdfText && textPipeline != VK_NULL_HANDLE;
      const bool wantsOpaquePipeline = !wantsSdfPipeline && batch.layer == volt::ui::UiBatchLayer::kOpaque && opaqueSolidPipeline != VK_NULL_HANDLE;
      VkPipeline pipeline = wantsSdfPipeline ? textPipeline : (wantsOpaquePipeline ? opaqueSolidPipeline : solidPipeline);
      if (pipeline == VK_NULL_HANDLE) {
        ++missingPipelineCount;
        continue;
      }

      if (pipeline != lastBoundPipeline) {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        lastBoundPipeline = pipeline;
      }

      double descriptorResolveMs = 0.0;
      const auto cachedDescriptor = resolvedDescriptorSets.find(textureKey);
      const VkDescriptorSet descriptorSet = cachedDescriptor != resolvedDescriptorSets.end()
          ? cachedDescriptor->second
          : [&]() {
              const auto descriptorResolveStart = std::chrono::steady_clock::now();
              const VkDescriptorSet resolvedDescriptor = resolveUiDescriptorSetForBatch(textureKey);
              const auto descriptorResolveEnd = std::chrono::steady_clock::now();
              descriptorResolveMs = std::chrono::duration<double, std::milli>(
                  descriptorResolveEnd - descriptorResolveStart).count();
              return resolvedDescriptor;
            }();
      if (cachedDescriptor == resolvedDescriptorSets.end()) {
        resolvedDescriptorSets.emplace(textureKey, descriptorSet);
      }
      descriptorResolveTotalMs += descriptorResolveMs;
      if (descriptorResolveMs > descriptorResolveMaxMs) {
        descriptorResolveMaxMs = descriptorResolveMs;
        descriptorResolveMaxTextureKey = textureKey;
      }
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

      UiSdfPushConstants uiConstants{};
      uiConstants.uiTransform = uiProjection;
      uiConstants.pxRange = std::max(1.0F, batch.sdfPxRange);
      uiConstants.edge = std::clamp(batch.sdfEdge, 0.35F, 0.65F);
      uiConstants.aaStrength = std::clamp(batch.sdfAaStrength, 0.1F, 0.6F);
      uiConstants.msdfMode = wantsSdfPipeline && batch.msdfText ? 1.0F : 0.0F;
      uiConstants.msdfConfidenceLow = std::clamp(batch.msdfConfidenceLow, 0.0F, 0.2F);
      uiConstants.msdfConfidenceHigh = std::clamp(batch.msdfConfidenceHigh, 0.0F, 0.25F);
      uiConstants.subpixelBlendStrength = std::clamp(batch.subpixelBlendStrength, 0.0F, 1.0F);
      uiConstants.smallTextSharpenStrength = std::clamp(batch.smallTextSharpenStrength, 0.0F, 1.0F);
      vkCmdPushConstants(
          commandBuffer,
          scaffoldPipelineLayout_,
          VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
          0,
          static_cast<uint32_t>(sizeof(UiSdfPushConstants)),
          &uiConstants);

      VkRect2D scissor{};
      scissor.offset.x = static_cast<std::int32_t>(std::max(0.0F, std::floor(batch.clipRect.x * scaleX)));
      scissor.offset.y = static_cast<std::int32_t>(std::max(0.0F, std::floor(batch.clipRect.y * scaleY)));
      scissor.extent.width = static_cast<std::uint32_t>(std::max(0.0F, std::ceil(batch.clipRect.width * scaleX)));
      scissor.extent.height = static_cast<std::uint32_t>(std::max(0.0F, std::ceil(batch.clipRect.height * scaleY)));

      if (static_cast<std::uint32_t>(scissor.offset.x) >= targetWidth || static_cast<std::uint32_t>(scissor.offset.y) >= targetHeight) {
        ++offscreenBatchCount;
        continue;
      }

      scissor.extent.width = std::min(scissor.extent.width, targetWidth - static_cast<std::uint32_t>(scissor.offset.x));
      scissor.extent.height = std::min(scissor.extent.height, targetHeight - static_cast<std::uint32_t>(scissor.offset.y));

      if (scissor.extent.width == 0U || scissor.extent.height == 0U) {
        ++zeroScissorBatchCount;
        continue;
      }

      drawnTextures.insert(textureKey);
      drawnClipRects.insert(clipKey);
      ++textureDrawCounts[textureKey];
      ++clipDrawCounts[clipKey];

      vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
      vkCmdDrawIndexed(commandBuffer, batch.indexCount, 1, batch.firstIndex, 0, 0);
      auto drawEnd = std::chrono::steady_clock::now();
      const double drawMs = std::chrono::duration<double, std::milli>(drawEnd - drawStart).count();
      if (drawMs > drawCallMaxMs) {
        drawCallMaxMs = drawMs;
        drawCallMaxBatchIndex = static_cast<std::int32_t>(batchIdx);
        drawCallMaxTextureKey = textureKey;
      }
      if (emitLogs && (drawMs >= kUiSlowPathWarnThresholdMs || descriptorResolveMs >= kUiSlowPathWarnThresholdMs)) {
        VOLT_LOG_WARN_CAT(
            volt::core::logging::Category::kRender,
            "UI DrawCall #", drawCallCount,
            " | batchIdx=", batchIdx,
            " | layer=", batch.layer == volt::ui::UiBatchLayer::kOpaque ? "opaque" : "transparent",
            " | textureKey=", batch.textureKey,
            " | sdfText=", batch.sdfText ? "true" : "false",
            " | msdfText=", batch.msdfText ? "true" : "false",
            " | clipRect=[", batch.clipRect.x, ",", batch.clipRect.y, ",", batch.clipRect.width, ",", batch.clipRect.height, "]",
            " | firstIndex=", batch.firstIndex,
            " | indexCount=", batch.indexCount,
            " | descriptorResolveMs=", descriptorResolveMs,
            " | cpuMs=", drawMs);
      }
      ++drawCallCount;
      drawCallTotalMs += drawMs;
    }

    if (emitLogs) {
        VOLT_LOG_DEBUG_CAT(
          volt::core::logging::Category::kRender,
          "UI BatchSummary | totalBatches=", meshData.batches.size(),
          " | drawCalls=", drawCallCount,
          " | uniqueTextures=", uniqueTextures.size(),
          " | drawnTextures=", drawnTextures.size(),
          " | uniqueClipRects=", uniqueClipRects.size(),
          " | drawnClipRects=", drawnClipRects.size(),
          " | skippedMissingPipeline=", missingPipelineCount,
          " | skippedOffscreen=", offscreenBatchCount,
          " | skippedZeroScissor=", zeroScissorBatchCount);

          VOLT_LOG_DEBUG_CAT(
            volt::core::logging::Category::kRender,
            "UI BatchTiming | drawAvgMs=", (drawCallCount > 0 ? (drawCallTotalMs / drawCallCount) : 0.0),
            " | drawMaxMs=", drawCallMaxMs,
            " | drawMaxBatchIdx=", drawCallMaxBatchIndex,
            " | drawMaxTexture=", drawCallMaxTextureKey,
            " | descriptorResolveTotalMs=", descriptorResolveTotalMs,
            " | descriptorResolveMaxMs=", descriptorResolveMaxMs,
            " | descriptorResolveMaxTexture=", descriptorResolveMaxTextureKey,
            " | vectorTextPrepMs=", latestFrameCpuTimings_.uiVectorTextPrepMs);

        VOLT_LOG_DEBUG_CAT(
          volt::core::logging::Category::kRender,
          "UI BatchTopTextures | batches=", topCountsString(textureBatchCounts, 5U),
          " | draws=", topCountsString(textureDrawCounts, 5U));

        VOLT_LOG_DEBUG_CAT(
          volt::core::logging::Category::kRender,
          "UI BatchTopClipRects | batches=", topCountsString(clipBatchCounts, 5U),
          " | draws=", topCountsString(clipDrawCounts, 5U));
    }

    if (updateFrameStats) {
      latestFrameCpuTimings_.uiDrawCallCount = drawCallCount;
      latestFrameCpuTimings_.uiDrawCallAvgMs = (drawCallCount > 0) ? (drawCallTotalMs / drawCallCount) : 0.0;
      latestFrameCpuTimings_.uiDrawCallMaxMs = drawCallMaxMs;
      latestFrameCpuTimings_.uiDrawCallMaxBatchIndex = drawCallMaxBatchIndex;
      latestFrameCpuTimings_.uiDrawCallMaxTextureKey = drawCallMaxTextureKey;
      latestFrameCpuTimings_.uiDescriptorResolveTotalMs = descriptorResolveTotalMs;
      latestFrameCpuTimings_.uiDescriptorResolveMaxMs = descriptorResolveMaxMs;
      latestFrameCpuTimings_.uiDescriptorResolveMaxTextureKey = descriptorResolveMaxTextureKey;
    }
  }

  void VulkanRenderer::recordUiMeshDraws(
      VkCommandBuffer commandBuffer,
      const volt::ui::UiMeshData& meshData)
  {
    const auto [logicalWidth, logicalHeight] = window_->logicalExtent();
    const float uiWidth = std::max(1.0F, static_cast<float>(logicalWidth == 0U ? swapchainExtent_.width : logicalWidth));
    const float uiHeight = std::max(1.0F, static_cast<float>(logicalHeight == 0U ? swapchainExtent_.height : logicalHeight));
    const volt::platform::DisplayMetrics metrics = window_->displayMetrics();
    const float scaleX = std::max(0.01F, metrics.contentScaleX);
    const float scaleY = std::max(0.01F, metrics.contentScaleY);

    const auto vectorTextPrepStart = std::chrono::steady_clock::now();
    prepareUiVectorTextBatches(meshData);
    const auto vectorTextPrepEnd = std::chrono::steady_clock::now();
    latestFrameCpuTimings_.uiVectorTextPrepMs = static_cast<std::int32_t>(std::lround(
      std::chrono::duration<double, std::milli>(vectorTextPrepEnd - vectorTextPrepStart).count()));
    recordUiMeshDrawsForExtent(
        commandBuffer,
        meshData,
        swapchainExtent_.width,
        swapchainExtent_.height,
        uiWidth,
        uiHeight,
        scaleX,
        scaleY,
        uiGraphicsPipeline_,
        uiOpaqueGraphicsPipeline_,
        uiTextSdfGraphicsPipeline_,
        true,
        true);
  }

  void VulkanRenderer::prepareUiRetainedPanels(const volt::ui::UiMeshData& meshData)
  {
    for (const volt::ui::UiRetainedPanel& panel : meshData.retainedPanels) {
      std::string error;
      if (!renderUiRetainedPanelToTexture(panel, error)) {
        VOLT_LOG_WARN_CAT(
            volt::core::logging::Category::kRender,
            "Failed to render retained panel to texture: ",
            panel.textureKey,
            " error=",
            error);
      }
    }
  }

  bool VulkanRenderer::renderUiRetainedPanelToTexture(const volt::ui::UiRetainedPanel& panel, std::string& outError)
  {
    outError.clear();

    if (uiOffscreenRenderPass_ == VK_NULL_HANDLE || uiOffscreenGraphicsPipeline_ == VK_NULL_HANDLE) {
      outError = "offscreen UI pipelines are not initialized";
      return false;
    }
    if (panel.vertices.empty() || panel.indices.empty() || panel.batches.empty()) {
      outError = "retained panel mesh is empty";
      return false;
    }

    const std::uint32_t imageWidth = std::max(1U, static_cast<std::uint32_t>(std::ceil(std::max(1.0F, panel.bounds.width))));
    const std::uint32_t imageHeight = std::max(1U, static_cast<std::uint32_t>(std::ceil(std::max(1.0F, panel.bounds.height))));

    const auto existingIt = uiTextureResources_.find(panel.textureKey);
    if (existingIt != uiTextureResources_.end() &&
        existingIt->second.retainedPanelSignature == panel.signature &&
        existingIt->second.width == imageWidth &&
        existingIt->second.height == imageHeight) {
      return true;
    }

    destroyUiTextureResource(panel.textureKey);

    UiTextureResource resource{};
    resource.retainedPanelSignature = panel.signature;
    resource.width = imageWidth;
    resource.height = imageHeight;
    auto cleanupRetainedResource = [&]() {
      releaseUiTextureResource(resource);
    };

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {imageWidth, imageHeight, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(device_, &imageInfo, nullptr, &resource.image) != VK_SUCCESS) {
      outError = "failed to create retained panel image";
      return false;
    }

    VkMemoryRequirements imageMemoryRequirements{};
    vkGetImageMemoryRequirements(device_, resource.image, &imageMemoryRequirements);
    VkMemoryAllocateInfo imageAllocInfo{};
    imageAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    imageAllocInfo.allocationSize = imageMemoryRequirements.size;
    imageAllocInfo.memoryTypeIndex = findMemoryType(imageMemoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device_, &imageAllocInfo, nullptr, &resource.imageMemory) != VK_SUCCESS ||
        vkBindImageMemory(device_, resource.image, resource.imageMemory, 0) != VK_SUCCESS) {
      cleanupRetainedResource();
      outError = "failed to allocate retained panel image memory";
      return false;
    }

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
      cleanupRetainedResource();
      outError = "failed to create retained panel image view";
      return false;
    }

    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = uiOffscreenRenderPass_;
    framebufferInfo.attachmentCount = 1;
    framebufferInfo.pAttachments = &resource.imageView;
    framebufferInfo.width = imageWidth;
    framebufferInfo.height = imageHeight;
    framebufferInfo.layers = 1;
    if (vkCreateFramebuffer(device_, &framebufferInfo, nullptr, &resource.framebuffer) != VK_SUCCESS) {
      cleanupRetainedResource();
      outError = "failed to create retained panel framebuffer";
      return false;
    }

    VkDescriptorSetAllocateInfo descriptorAllocInfo{};
    descriptorAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descriptorAllocInfo.descriptorPool = uiDescriptorPool_;
    descriptorAllocInfo.descriptorSetCount = 1;
    descriptorAllocInfo.pSetLayouts = &uiDescriptorSetLayout_;
    if (vkAllocateDescriptorSets(device_, &descriptorAllocInfo, &resource.descriptorSet) != VK_SUCCESS) {
      cleanupRetainedResource();
      outError = "failed to allocate retained panel descriptor set";
      return false;
    }

    VkDescriptorImageInfo sampledImageInfo{};
    sampledImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    sampledImageInfo.imageView = resource.imageView;
    sampledImageInfo.sampler = uiTextureSampler_;

    VkWriteDescriptorSet graphicsWrite{};
    graphicsWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    graphicsWrite.dstSet = resource.descriptorSet;
    graphicsWrite.dstBinding = 0;
    graphicsWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    graphicsWrite.descriptorCount = 1;
    graphicsWrite.pImageInfo = &sampledImageInfo;
    vkUpdateDescriptorSets(device_, 1, &graphicsWrite, 0, nullptr);

    volt::ui::UiMeshData retainedMesh{};
    retainedMesh.vertices = panel.vertices;
    retainedMesh.indices = panel.indices;
    retainedMesh.batches = panel.batches;
    uploadUiMeshData(retainedMesh);

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo commandAllocInfo{};
    commandAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandAllocInfo.commandPool = scaffoldCommandPool_;
    commandAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandAllocInfo.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device_, &commandAllocInfo, &commandBuffer) != VK_SUCCESS) {
      cleanupRetainedResource();
      outError = "failed to allocate retained panel command buffer";
      return false;
    }

    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(device_, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
      vkFreeCommandBuffers(device_, scaffoldCommandPool_, 1, &commandBuffer);
      cleanupRetainedResource();
      outError = "failed to create retained panel fence";
      return false;
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
      vkDestroyFence(device_, fence, nullptr);
      vkFreeCommandBuffers(device_, scaffoldCommandPool_, 1, &commandBuffer);
      cleanupRetainedResource();
      outError = "failed to begin retained panel command buffer";
      return false;
    }

    VkImageMemoryBarrier toColorAttachment{};
    toColorAttachment.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toColorAttachment.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toColorAttachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toColorAttachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toColorAttachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toColorAttachment.image = resource.image;
    toColorAttachment.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toColorAttachment.subresourceRange.baseMipLevel = 0;
    toColorAttachment.subresourceRange.levelCount = 1;
    toColorAttachment.subresourceRange.baseArrayLayer = 0;
    toColorAttachment.subresourceRange.layerCount = 1;
    toColorAttachment.srcAccessMask = 0;
    toColorAttachment.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &toColorAttachment);

    VkClearValue clearValue{};
    clearValue.color = {{0.0F, 0.0F, 0.0F, 0.0F}};
    VkRenderPassBeginInfo renderPassBeginInfo{};
    renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassBeginInfo.renderPass = uiOffscreenRenderPass_;
    renderPassBeginInfo.framebuffer = resource.framebuffer;
    renderPassBeginInfo.renderArea.offset = {0, 0};
    renderPassBeginInfo.renderArea.extent = {imageWidth, imageHeight};
    renderPassBeginInfo.clearValueCount = 1;
    renderPassBeginInfo.pClearValues = &clearValue;
    vkCmdBeginRenderPass(commandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

    recordUiMeshDrawsForExtent(
        commandBuffer,
        retainedMesh,
        imageWidth,
        imageHeight,
        std::max(1.0F, panel.bounds.width),
        std::max(1.0F, panel.bounds.height),
        1.0F,
        1.0F,
        uiOffscreenGraphicsPipeline_,
        VK_NULL_HANDLE,
        uiOffscreenTextSdfGraphicsPipeline_,
        false,
        false);

    vkCmdEndRenderPass(commandBuffer);

    VkImageMemoryBarrier toShaderRead{};
    toShaderRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toShaderRead.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toShaderRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toShaderRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShaderRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShaderRead.image = resource.image;
    toShaderRead.subresourceRange = toColorAttachment.subresourceRange;
    toShaderRead.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toShaderRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &toShaderRead);

    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
      vkDestroyFence(device_, fence, nullptr);
      vkFreeCommandBuffers(device_, scaffoldCommandPool_, 1, &commandBuffer);
      cleanupRetainedResource();
      outError = "failed to end retained panel command buffer";
      return false;
    }

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    if (vkQueueSubmit(graphicsQueue_, 1, &submitInfo, fence) != VK_SUCCESS) {
      vkDestroyFence(device_, fence, nullptr);
      vkFreeCommandBuffers(device_, scaffoldCommandPool_, 1, &commandBuffer);
      cleanupRetainedResource();
      outError = "failed to submit retained panel command buffer";
      return false;
    }
    if (vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
      vkDestroyFence(device_, fence, nullptr);
      vkFreeCommandBuffers(device_, scaffoldCommandPool_, 1, &commandBuffer);
      cleanupRetainedResource();
      outError = "failed to wait for retained panel fence";
      return false;
    }

    vkDestroyFence(device_, fence, nullptr);
    vkFreeCommandBuffers(device_, scaffoldCommandPool_, 1, &commandBuffer);
    uiTextureResources_[panel.textureKey] = resource;
    return true;
  }

  VkDescriptorSet VulkanRenderer::resolveUiDescriptorSetForBatch(const std::string& textureKey)
  {
    const std::string key = textureKey.empty() ? "__white" : textureKey;

    const std::string& defaultFontTextureKey = volt::io::defaultFontTextureKey();
    if (key == defaultFontTextureKey) {
      const std::uint64_t currentFontAtlasRevision = volt::io::defaultFontAtlasRevision();
      auto fontIt = uiTextureResources_.find(key);
      if (fontIt != uiTextureResources_.end() && fontIt->second.fontAtlasRevision != currentFontAtlasRevision) {
        destroyUiTextureResource(key);
        unresolvedTextureKeysLogged_.erase(key);
        fontIt = uiTextureResources_.end();
      }

      if (fontIt == uiTextureResources_.end()) {
        createUiTextureResourceForKey(key);
        fontIt = uiTextureResources_.find(key);
      }

      if (fontIt != uiTextureResources_.end()) {
        return fontIt->second.descriptorSet;
      }

      return uiDescriptorSet_;
    }

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
    std::vector<const char *> requiredExtensions = {
        VK_KHR_SURFACE_EXTENSION_NAME,
    };

    switch (window_->backendType()) {
      case volt::platform::WindowBackendType::kWin32:
#if defined(_WIN32)
        requiredExtensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
        break;
#else
        throw std::runtime_error("Win32 window backend is unavailable in this build");
#endif
      case volt::platform::WindowBackendType::kLinux:
        throw std::runtime_error("Linux Vulkan surface extensions are not implemented yet");
      case volt::platform::WindowBackendType::kMacOS:
        throw std::runtime_error("macOS Vulkan surface extensions are not implemented yet");
      case volt::platform::WindowBackendType::kUnknown:
      default:
        throw std::runtime_error("Unsupported window backend for Vulkan instance creation");
    }

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
    switch (window_->backendType()) {
      case volt::platform::WindowBackendType::kWin32:
#if defined(_WIN32)
      {
        auto createWin32Surface = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
            vkGetInstanceProcAddr(instance_, "vkCreateWin32SurfaceKHR"));
        if (createWin32Surface == nullptr) {
          throw std::runtime_error("vkCreateWin32SurfaceKHR is unavailable");
        }

        VkWin32SurfaceCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        createInfo.hinstance = static_cast<HINSTANCE>(window_->nativeDisplayHandle());
        createInfo.hwnd = static_cast<HWND>(window_->nativeWindowHandle());

        if (createInfo.hinstance == nullptr || createInfo.hwnd == nullptr) {
          throw std::runtime_error("Window backend returned invalid Win32 surface handles");
        }

        const VkResult result = createWin32Surface(instance_, &createInfo, nullptr, &surface_);
        if (result != VK_SUCCESS) {
          throw std::runtime_error("Failed to create Vulkan Win32 surface");
        }

        break;
      }
#else
        throw std::runtime_error("Win32 surface creation is unavailable in this build");
#endif
      case volt::platform::WindowBackendType::kLinux:
        throw std::runtime_error("Linux Vulkan surface creation is not implemented yet");
      case volt::platform::WindowBackendType::kMacOS:
        throw std::runtime_error("macOS Vulkan surface creation is not implemented yet");
      case volt::platform::WindowBackendType::kUnknown:
      default:
        throw std::runtime_error("Unsupported window backend for Vulkan surface creation");
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

    uint32_t imageCount = std::max(support.capabilities.minImageCount + 1, kMaxFramesInFlight);
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

    VOLT_LOG_INFO_CAT(
      volt::core::logging::Category::kRender,
      "Swapchain present mode: ",
      presentModeName(presentMode),
      " | images=",
      imageCount,
      " | extent=",
      extent.width,
      "x",
      extent.height);
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

    auto [width, height] = window_->framebufferExtent();

    while (width == 0U || height == 0U)
    {
      window_->waitEvents();
      const auto framebufferExtent = window_->framebufferExtent();
      width = framebufferExtent.first;
      height = framebufferExtent.second;
    }

    vkDeviceWaitIdle(device_);
    for (std::uint32_t frameSlot = 0; frameSlot < retiredUiTextureResources_.size(); ++frameSlot) {
      releaseRetiredUiTextureResources(frameSlot);
      releaseRetiredBufferResources(frameSlot);
    }
    cleanupSwapchain();
    cleanupUiTextSdfGraphicsPipeline();
    cleanupUiOpaqueGraphicsPipeline();
    cleanupUiGraphicsPipeline();
    cleanupUiOffscreenTextSdfGraphicsPipeline();
    cleanupUiOffscreenGraphicsPipeline();
    cleanupUiOffscreenRenderPass();
    cleanupScaffoldGraphicsPipeline();
    cleanupScaffoldRenderPass();
    createSwapchain();
    createSwapchainImageViews();
    createScaffoldDepthResources();
    createScaffoldRenderPass();
    createUiOffscreenRenderPass();
    createScaffoldGraphicsPipeline();
    createUiGraphicsPipeline();
    createUiOpaqueGraphicsPipeline();
    createUiTextSdfGraphicsPipeline();
    createUiOffscreenGraphicsPipeline();
    createUiOffscreenTextSdfGraphicsPipeline();
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
    const std::string configuredModeName = configuredPresentModeName();
    if (!configuredModeName.empty()) {
      const auto parsedPresentMode = parsePresentModeName(configuredModeName);
      if (parsedPresentMode.has_value()) {
        if (presentModeAvailable(availablePresentModes, *parsedPresentMode)) {
          return *parsedPresentMode;
        }

        VOLT_LOG_WARN_CAT(
            volt::core::logging::Category::kRender,
            "Configured present mode unavailable, falling back to automatic selection: ",
            configuredModeName);
      } else if (!equalsAsciiCaseInsensitive(configuredModeName, "auto")) {
        VOLT_LOG_WARN_CAT(
            volt::core::logging::Category::kRender,
            "Ignoring unknown present mode configuration: ",
            configuredModeName);
      }
    }

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

    const auto [width, height] = window_->framebufferExtent();

    VkExtent2D actualExtent = {
      width,
      height,
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

} // namespace volt::render::details
