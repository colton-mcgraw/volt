#include "volt/core/AppConfig.hpp"
#include "volt/core/Logging.hpp"
#include "volt/core/Timer.hpp"
#include "volt/event/Event.hpp"
#include "volt/event/EventDispatcher.hpp"
#include "volt/io/ImageCodec.hpp"
#include "volt/io/ImporterRegistry.hpp"
#include "volt/io/ImportPipeline.hpp"
#include "volt/math/Math.hpp"
#include "volt/platform/Window.hpp"
#include "volt/render/VulkanRenderer.hpp"
#include "volt/ui/UILayer.hpp"

#include <exception>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

#include <vulkan/vulkan.h>

static std::atomic_int32_t g_TickCount{0};
static std::atomic_bool g_Running{true};

static VkRect2D clipRectToFramebuffer(const volt::ui::Rect& bounds, std::uint32_t width, std::uint32_t height) {
  const float x0 = std::clamp(bounds.x, 0.0F, static_cast<float>(width));
  const float y0 = std::clamp(bounds.y, 0.0F, static_cast<float>(height));
  const float x1 = std::clamp(bounds.x + bounds.width, 0.0F, static_cast<float>(width));
  const float y1 = std::clamp(bounds.y + bounds.height, 0.0F, static_cast<float>(height));

  VkRect2D scissor{};
  scissor.offset.x = static_cast<std::int32_t>(x0);
  scissor.offset.y = static_cast<std::int32_t>(y0);
  scissor.extent.width = static_cast<std::uint32_t>(std::max(0.0F, x1 - x0));
  scissor.extent.height = static_cast<std::uint32_t>(std::max(0.0F, y1 - y0));
  return scissor;
}

static const char* eventTypeToString(volt::event::EventType type) {
  switch (type) {
    case volt::event::EventType::kFrameStarted:
      return "FrameStarted";
    case volt::event::EventType::kFrameEnded:
      return "FrameEnded";
    case volt::event::EventType::kWindowResized:
      return "WindowResized";
    case volt::event::EventType::kWindowMinimized:
      return "WindowMinimized";
    case volt::event::EventType::kKeyInput:
      return "KeyInput";
    case volt::event::EventType::kMouseMoved:
      return "MouseMoved";
    case volt::event::EventType::kMouseButton:
      return "MouseButton";
    case volt::event::EventType::kMouseScrolled:
      return "MouseScrolled";
    case volt::event::EventType::kRenderFrameBegin:
      return "RenderFrameBegin";
    case volt::event::EventType::kRenderScenePassBegin:
      return "RenderScenePassBegin";
    case volt::event::EventType::kRenderScenePassEnd:
      return "RenderScenePassEnd";
    case volt::event::EventType::kRenderUiPass:
      return "RenderUiPass";
    case volt::event::EventType::kRenderFrameEnd:
      return "RenderFrameEnd";
    case volt::event::EventType::kUiFrameBegin:
      return "UiFrameBegin";
    case volt::event::EventType::kUiLayoutPass:
      return "UiLayoutPass";
    case volt::event::EventType::kUiPaintPass:
      return "UiPaintPass";
    case volt::event::EventType::kUiFrameEnd:
      return "UiFrameEnd";
    case volt::event::EventType::kImportStarted:
      return "ImportStarted";
    case volt::event::EventType::kImportStageComplete:
      return "ImportStageComplete";
    case volt::event::EventType::kImportSucceeded:
      return "ImportSucceeded";
    case volt::event::EventType::kImportFailed:
      return "ImportFailed";
    case volt::event::EventType::kUnknown:
    default:
      return "Unknown";
  }
}

static void logTicksToConsole() {
  const int32_t tick = g_TickCount.load(std::memory_order_relaxed);
  g_TickCount.store(0, std::memory_order_relaxed);
  VOLT_LOG_TRACE_CAT(volt::core::logging::Category::kApp, "Tick ", tick);
  std::this_thread::sleep_for(std::chrono::seconds(1));
}

static bool createCodecRenderSmokeAssets() {
  constexpr std::uint32_t kWidth = 128;
  constexpr std::uint32_t kHeight = 128;

  volt::io::DecodedImage image{};
  image.width = kWidth;
  image.height = kHeight;
  image.rgba.resize(static_cast<std::size_t>(kWidth) * static_cast<std::size_t>(kHeight) * 4U);

  for (std::uint32_t y = 0; y < kHeight; ++y) {
    for (std::uint32_t x = 0; x < kWidth; ++x) {
      const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(kWidth) + x) * 4U;
      const bool checker = ((x / 16U) + (y / 16U)) % 2U == 0U;
      image.rgba[i + 0U] = checker ? 250U : 32U;
      image.rgba[i + 1U] = static_cast<std::uint8_t>((x * 255U) / kWidth);
      image.rgba[i + 2U] = static_cast<std::uint8_t>((y * 255U) / kHeight);
      image.rgba[i + 3U] = 255U;
    }
  }

  const std::filesystem::path dir = std::filesystem::path("assets") / "images";
  const std::filesystem::path png = dir / "codec-render-test.png";
  const std::filesystem::path jpg = dir / "codec-render-test.jpg";
  const std::filesystem::path bmp = dir / "codec-render-test.bmp";

  const bool wrotePng = volt::io::encodeImageFile(png, image, volt::io::ImageEncodeFormat::kPng);
  const bool wroteJpg = volt::io::encodeImageFile(jpg, image, volt::io::ImageEncodeFormat::kJpeg, 92);
  const bool wroteBmp = volt::io::encodeImageFile(bmp, image, volt::io::ImageEncodeFormat::kBmp);

  volt::io::DecodedImage decoded{};
  const bool decodedPng = wrotePng && volt::io::decodeImageFile(png, decoded);
  const bool decodedJpg = wroteJpg && volt::io::decodeImageFile(jpg, decoded);
  const bool decodedBmp = wroteBmp && volt::io::decodeImageFile(bmp, decoded);

  VOLT_LOG_INFO_CAT(
      volt::core::logging::Category::kIO,
      "Image codec smoke test: png=",
      decodedPng ? "ok" : "fail",
      " jpg=",
      decodedJpg ? "ok" : "fail",
      " bmp=",
      decodedBmp ? "ok" : "fail");

  return decodedPng && decodedJpg && decodedBmp;
}

int main() {
  try {
    volt::core::logging::initialize();
    VOLT_LOG_INFO_CAT(
        volt::core::logging::Category::kApp,
        "Logging config: level=",
        volt::core::logging::configuredLevelName(),
        " categories=",
        volt::core::logging::enabledCategoriesSummary(),
        " eventTrace=",
        volt::core::logging::isFeatureEnabled(volt::core::logging::Feature::kEventTrace) ? "on" : "off",
        " tickTrace=",
        volt::core::logging::isFeatureEnabled(volt::core::logging::Feature::kTickTrace) ? "on" : "off");

      const bool inverseCheck = volt::math::validateMat4Inverse<float>();
      const bool quaternionRoundTripCheck = volt::math::validateQuaternionMatrixRoundTrip<float>();
      VOLT_LOG_INFO_CAT(
        volt::core::logging::Category::kApp,
        "Math validation inverse=",
        inverseCheck ? "pass" : "fail",
        " quatRoundTrip=",
        quaternionRoundTripCheck ? "pass" : "fail");

    const volt::core::AppConfig config{};
    std::uint64_t frameIndex = 0;

    volt::event::EventDispatcher eventDispatcher;

    if (volt::core::logging::isFeatureEnabled(volt::core::logging::Feature::kEventTrace)) {
      eventDispatcher.subscribe([](const volt::event::Event& event) {
        VOLT_LOG_DEBUG_CAT(
            volt::core::logging::Category::kEvent,
            "[event ",
            event.sequence,
            "] ",
            eventTypeToString(event.type));
      });
    }

    std::thread tickThread;
    if (volt::core::logging::isFeatureEnabled(volt::core::logging::Feature::kTickTrace)) {
      tickThread = std::thread([&]() {
        while (g_Running.load(std::memory_order_relaxed)) {
          logTicksToConsole();
        }
      });
    }

    volt::io::ImporterRegistry importers;
    importers.registerDefaultImporters();
    volt::io::ImportPipeline importPipeline;
    importPipeline.setEventDispatcher(&eventDispatcher);

    const bool codecSmokeOk = createCodecRenderSmokeAssets();
    VOLT_LOG_INFO_CAT(
      volt::core::logging::Category::kApp,
      "Codec render smoke setup ",
      codecSmokeOk ? "ready" : "degraded");

    volt::platform::Window window(config.windowWidth, config.windowHeight, config.appName);
    volt::render::VulkanRenderer renderer(window.nativeHandle(), config.appName.c_str());
    volt::ui::UILayer uiLayer;

    window.setEventDispatcher(&eventDispatcher);
    renderer.setEventDispatcher(&eventDispatcher);
    renderer.setUiMeshProvider([&uiLayer]() {
      return &uiLayer.meshData();
    });
    uiLayer.setEventDispatcher(&eventDispatcher);
    renderer.setUiPassCallback(
        [&uiLayer](VkCommandBuffer commandBuffer, std::uint32_t framebufferWidth, std::uint32_t framebufferHeight) {
          uiLayer.recordRenderPass(commandBuffer, framebufferWidth, framebufferHeight);

          for (const volt::ui::UiMeshBatch& batch : uiLayer.meshData().batches) {
            const VkRect2D scissor = clipRectToFramebuffer(batch.clipRect, framebufferWidth, framebufferHeight);
            if (scissor.extent.width == 0U || scissor.extent.height == 0U) {
              continue;
            }

            vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
          }
        });

    constexpr double kLogicTickSeconds = 1.0 / 60.0;
    constexpr double kMinimizedWakeSeconds = 0.1;
    constexpr int kMaxLogicTicksPerWake = 8;

    bool firstFrame = true;
    auto lastWake = std::chrono::steady_clock::now();
    double logicAccumulatorSeconds = 0.0;
    bool renderDirty = true;
    bool wasMinimized = window.isMinimized();

    while (!window.shouldClose()) {
      const volt::core::Timer frameTimer;

      if (!firstFrame) {
        const double waitSeconds = window.isMinimized()
                                       ? kMinimizedWakeSeconds
                                       : std::max(0.0, kLogicTickSeconds - logicAccumulatorSeconds);
        window.waitEventsTimeout(waitSeconds);

        if (window.shouldClose()) {
          break;
        }
      }
      firstFrame = false;

      const auto now = std::chrono::steady_clock::now();
      const double deltaSeconds = std::chrono::duration<double>(now - lastWake).count();
      lastWake = now;

      logicAccumulatorSeconds += deltaSeconds;
      const double maxAccumulated = kLogicTickSeconds * static_cast<double>(kMaxLogicTicksPerWake);
      if (logicAccumulatorSeconds > maxAccumulated) {
        logicAccumulatorSeconds = maxAccumulated;
      }

      eventDispatcher.dispatchQueued();

      const bool minimizedNow = window.isMinimized();
      if (minimizedNow != wasMinimized) {
        if (!minimizedNow) {
          renderDirty = true;
        }
        wasMinimized = minimizedNow;
      }

      int logicTicksThisWake = 0;
      while (logicAccumulatorSeconds >= kLogicTickSeconds && logicTicksThisWake < kMaxLogicTicksPerWake) {
        logicAccumulatorSeconds -= kLogicTickSeconds;
        ++logicTicksThisWake;
      }

      if (minimizedNow) {
        g_TickCount.fetch_add(logicTicksThisWake, std::memory_order_relaxed);
        continue;
      }

      const auto& input = window.inputSnapshot();
      const bool hasPointerInput =
          std::abs(input.mouse.deltaX) > 0.0 || std::abs(input.mouse.deltaY) > 0.0 ||
          std::abs(input.mouse.scrollX) > 0.0 || std::abs(input.mouse.scrollY) > 0.0;
      const bool hasMouseButtonDown =
          std::any_of(input.mouse.down.begin(), input.mouse.down.end(), [](bool down) { return down; });
      const bool hasKeyboardDown =
          std::any_of(input.keyboard.down.begin(), input.keyboard.down.end(), [](bool down) { return down; });
      const bool resizedThisWake = window.wasResized();
      const bool shouldRenderFrame =
          renderDirty || resizedThisWake || hasPointerInput || hasMouseButtonDown || hasKeyboardDown;

      if (!shouldRenderFrame) {
        g_TickCount.fetch_add(logicTicksThisWake, std::memory_order_relaxed);
        continue;
      }

      eventDispatcher.enqueue({
          .type = volt::event::EventType::kFrameStarted,
          .payload = volt::event::FrameLifecycleEvent{.frameIndex = frameIndex},
      });
      eventDispatcher.dispatchQueued();

      const auto [framebufferWidth, framebufferHeight] = window.framebufferExtent();
      const volt::ui::UILayer::FrameArgs uiFrameArgs{
          framebufferWidth,
          framebufferHeight,
          window.isMinimized(),
      };

      uiLayer.beginFrame(window.inputSnapshot(), uiFrameArgs);
        uiLayer.beginPanel(
          {16.0F, 12.0F, 340.0F, 360.0F},
          volt::ui::PanelElement{"left_panel", {0.08F, 0.10F, 0.12F, 1.0F}, 10.0F});
        uiLayer.beginFlowColumn({24.0F, 20.0F, 320.0F, 340.0F}, 10.0F, 8.0F);
        uiLayer.addTextFlow(28.0F, volt::ui::TextElement{"Volt UI Foundation", "default", 18.0F, {0.95F, 0.97F, 1.0F, 1.0F}});
        uiLayer.beginFlowRow({32.0F, 54.0F, 300.0F, 36.0F}, 24.0F, 8.0F, 0.0F);
        uiLayer.addTextRow(180.0F, volt::ui::TextElement{"Actions", "default", 13.0F, {0.70F, 0.74F, 0.80F, 1.0F}});
        uiLayer.addIconRow(24.0F, volt::ui::IconElement{"icon:warning", {0.96F, 0.78F, 0.22F, 1.0F}});
        uiLayer.endFlowRow();
        uiLayer.addButtonFlow(34.0F, volt::ui::ButtonElement{"Import", true, false, {0.20F, 0.31F, 0.45F, 1.0F}, {1.0F, 1.0F, 1.0F, 1.0F}});
        uiLayer.addSliderFlow(18.0F, volt::ui::SliderElement{0.0F, 100.0F, 38.0F, {0.17F, 0.19F, 0.22F, 1.0F}, {0.85F, 0.88F, 0.93F, 1.0F}});
        uiLayer.addImageFlow(70.0F, volt::ui::ImageElement{"image:preview-board", {1.0F, 1.0F, 1.0F, 1.0F}});
        uiLayer.addImageFlow(70.0F, volt::ui::ImageElement{"image:codec-render-test", {1.0F, 1.0F, 1.0F, 1.0F}});
        uiLayer.endFlowColumn();
        uiLayer.endPanel();
        uiLayer.addChartScaffold(
          {24.0F, 150.0F, 320.0F, 160.0F},
          volt::ui::ChartScaffoldElement{"line", {0.2F, 0.5F, 0.7F, 0.45F, 0.9F}});
        uiLayer.addSchematicScaffold(
          {360.0F, 150.0F, 360.0F, 220.0F},
          volt::ui::SchematicScaffoldElement{"power_stage_topology", 24U, 38U});
      uiLayer.layoutPass();
      uiLayer.paintPass();

      renderer.submitScene({0U, 0U});
      renderer.tick(false);
      eventDispatcher.dispatchQueued();

      uiLayer.endFrame();

      eventDispatcher.enqueue({
          .type = volt::event::EventType::kFrameEnded,
          .payload = volt::event::FrameLifecycleEvent{.frameIndex = frameIndex},
      });
      eventDispatcher.dispatchQueued();
      ++frameIndex;

      if (resizedThisWake) {
        window.acknowledgeResize();
      }
      renderDirty = false;

      VOLT_LOG_TRACE_CAT(
          volt::core::logging::Category::kApp,
          "Frame ",
          frameIndex,
          " logicTicks=",
          logicTicksThisWake,
          " durationMs=",
          frameTimer.elapsedMilliseconds());

      g_TickCount.fetch_add(logicTicksThisWake, std::memory_order_relaxed);
    }

    g_Running.store(false, std::memory_order_relaxed);
    if (tickThread.joinable()) {
      tickThread.join();
    }

    volt::core::logging::shutdown();

    return 0;
  } catch (const std::exception& ex) {
    VOLT_LOG_CRITICAL_CAT(volt::core::logging::Category::kApp, "Fatal error: ", ex.what());
    std::cerr << "Fatal error: " << ex.what() << '\n';
    volt::core::logging::shutdown();
    return 1;
  }
}
