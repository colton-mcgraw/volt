#include "AppUi.hpp"
#include "setup_app.hpp"

#include "volt/core/AppConfig.hpp"
#include "volt/core/Logging.hpp"
#include "volt/core/Timer.hpp"
#include "volt/event/Event.hpp"
#include "volt/event/EventDispatcher.hpp"
#include "volt/io/import/ImporterRegistry.hpp"
#include "volt/io/import/ImportPipeline.hpp"
#include "volt/platform/Window.hpp"
#include "volt/render/RendererFactory.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

static std::atomic_int32_t g_TickCount{0};
static std::atomic_int32_t g_lastFrameTimeMs{0};
static std::atomic_int32_t g_lastEventWaitTimeMs{0};
static std::atomic_int32_t g_lastUiBuildTimeMs{0};
static std::atomic_int32_t g_lastRendererTickTimeMs{0};
static std::atomic_int32_t g_lastRenderFrameCpuTimeMs{0};
static std::atomic_bool g_Running{true};

static void logTicksToConsole() {
  const int32_t tick = g_TickCount.load(std::memory_order_relaxed);
  g_TickCount.store(0, std::memory_order_relaxed);
  VOLT_LOG_TRACE_CAT(volt::core::logging::Category::kApp, "Tick ", tick);
  printf("Frames in the last second: %d\n", tick);
  std::this_thread::sleep_for(std::chrono::seconds(1));
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

    const volt::core::AppConfig config{};
    std::uint64_t frameIndex = 0;

    volt::event::EventDispatcher eventDispatcher;

    if (volt::core::logging::isFeatureEnabled(volt::core::logging::Feature::kEventTrace)) {
      eventDispatcher.subscribe([](const volt::event::Event& event) {
        (void)event;
        VOLT_LOG_DEBUG_CAT(
            volt::core::logging::Category::kEvent,
            "[event ",
            event.sequence,
            "] ",
            volt::app::eventTypeToString(event.type));
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

    const std::string windowTitle = config.windowTitle.empty() ? config.appName : config.windowTitle;
    const volt::platform::WindowCreateOptions windowOptions{
      .useSystemTitleBar = config.useSystemTitleBar,
      .useSystemResizeHandles = config.useSystemResizeHandles,
      .windowResizable = config.windowResizable,
      .initialX = config.windowX,
      .initialY = config.windowY,
      .resizeBorderThickness = config.windowResizeBorderThickness,
      .titleBarHeight = config.windowTitleBarHeight,
      .titleBarPadding = config.windowTitleBarPadding,
      .titleBarButtonSize = config.windowTitleBarButtonSize,
      .titleBarButtonSpacing = config.windowTitleBarButtonSpacing,
      .windowIconPath = config.windowIconPath,
    };
    volt::platform::Window window(config.windowWidth, config.windowHeight, windowTitle, windowOptions);
    std::unique_ptr<volt::render::Renderer> renderer =
        volt::render::createRenderer(window, config.appName.c_str());
    volt::ui::UILayer uiLayer;

    window.setEventDispatcher(&eventDispatcher);
    renderer->setEventDispatcher(&eventDispatcher);
    renderer->setUiMeshProvider([&uiLayer]() {
      return &uiLayer.meshData();
    });
    uiLayer.setEventDispatcher(&eventDispatcher);

    constexpr double kLogicTickSeconds = 1.0 / 60.0;
    constexpr int kMaxLogicTicksPerWake = 8;
    constexpr auto kResizeRenderHold = std::chrono::milliseconds(250);

    auto lastWake = std::chrono::steady_clock::now();
    auto continuousRenderUntil = std::chrono::steady_clock::time_point{};
    double logicAccumulatorSeconds = 0.0;
    bool renderDirty = true;
    bool callbackRepaintInProgress = false;
    bool wasMinimized = window.isMinimized();
    bool leftMousePressedThisWake = false;
    bool previousLeftMouseDown = false;
    volt::app::AppUiState uiState{};

    for (const volt::event::EventType type : std::array{
             volt::event::EventType::kWindowResized,
             volt::event::EventType::kWindowMinimized,
             volt::event::EventType::kKeyInput,
             volt::event::EventType::kTextInput,
             volt::event::EventType::kMouseMoved,
             volt::event::EventType::kMouseButton,
             volt::event::EventType::kMouseScrolled,
             volt::event::EventType::kImportStarted,
             volt::event::EventType::kImportStageComplete,
             volt::event::EventType::kImportSucceeded,
             volt::event::EventType::kImportFailed,
         }) {
      eventDispatcher.subscribe(type, [&, type](const volt::event::Event& event) {
        renderDirty = true;
        if (type != volt::event::EventType::kMouseButton) {
          return;
        }

        const auto* payload = std::get_if<volt::event::MouseButtonEvent>(&event.payload);
        if (payload != nullptr && payload->button == 0 && payload->action == 1) {
          leftMousePressedThisWake = true;
        }
      });
    }

    auto renderFrameNow = [&](bool forceResize) -> bool {
      bool requestFollowUpFrame = false;
      const volt::core::Timer renderFrameCpuTimer;
      if (window.isMinimized()) {
        return false;
      }

      const auto [framebufferWidth, framebufferHeight] = window.framebufferExtent();
      const auto [logicalWidth, logicalHeight] = window.logicalExtent();
      if (framebufferWidth == 0U || framebufferHeight == 0U || logicalWidth == 0U || logicalHeight == 0U) {
        return false;
      }

      eventDispatcher.enqueue({
          .type = volt::event::EventType::kFrameStarted,
          .payload = volt::event::FrameLifecycleEvent{.frameIndex = frameIndex},
      });
      eventDispatcher.dispatchQueued();

      const volt::ui::UILayer::FrameArgs uiFrameArgs{
          logicalWidth,
          logicalHeight,
          window.isMinimized(),
      };
        const auto rendererFrameCpuTimings = renderer->frameCpuTimings();
        const auto vectorTextGpuTimings = renderer->vectorTextGpuTimings();
      const volt::core::Timer uiBuildTimer;
        const volt::app::AppUiFrameStats frameStats{
          g_lastFrameTimeMs.load(std::memory_order_relaxed),
          g_lastEventWaitTimeMs.load(std::memory_order_relaxed),
          g_lastUiBuildTimeMs.load(std::memory_order_relaxed),
          g_lastRendererTickTimeMs.load(std::memory_order_relaxed),
          g_lastRenderFrameCpuTimeMs.load(std::memory_order_relaxed),
          rendererFrameCpuTimings.valid,
          rendererFrameCpuTimings.fenceWaitMs,
          rendererFrameCpuTimings.acquireMs,
          rendererFrameCpuTimings.recordMs,
          rendererFrameCpuTimings.recordUploadMs,
          rendererFrameCpuTimings.recordDrawMs,
          rendererFrameCpuTimings.uiVectorTextPrepMs,
          rendererFrameCpuTimings.submitMs,
          rendererFrameCpuTimings.presentMs,
          vectorTextGpuTimings.valid,
          vectorTextGpuTimings.flattenCountMs,
          vectorTextGpuTimings.curveScanMs,
          vectorTextGpuTimings.flattenEmitBinCountMs,
          vectorTextGpuTimings.tileScanMs,
          vectorTextGpuTimings.binEmitFineMs,
          rendererFrameCpuTimings.uiDrawCallCount,
          rendererFrameCpuTimings.uiDrawCallAvgMs,
          rendererFrameCpuTimings.uiDrawCallMaxMs,
          rendererFrameCpuTimings.uiDrawCallMaxBatchIndex,
          rendererFrameCpuTimings.uiDrawCallMaxTextureKey,
          rendererFrameCpuTimings.uiDescriptorResolveTotalMs,
          rendererFrameCpuTimings.uiDescriptorResolveMaxMs,
          rendererFrameCpuTimings.uiDescriptorResolveMaxTextureKey,
        };
      const volt::app::AppUiFrameBindings uiBindings =
          volt::app::buildAppUi(uiLayer, window, uiFrameArgs, frameStats, uiState);
      uiLayer.layoutPass();
      g_lastUiBuildTimeMs.store(static_cast<int32_t>(std::round(uiBuildTimer.elapsedMilliseconds())), std::memory_order_relaxed);

      const bool leftMouseDown = window.inputSnapshot().mouse.down[0];
      const bool leftMousePressed = leftMousePressedThisWake || (leftMouseDown && !previousLeftMouseDown);
      const std::uint64_t hoveredId = uiLayer.hoveredWidgetId();

      requestFollowUpFrame =
          volt::app::handleAppUiInteractions(window, uiBindings, uiState, hoveredId, leftMousePressed);

      uiLayer.paintPass();
      previousLeftMouseDown = leftMouseDown;
      leftMousePressedThisWake = false;

        const volt::core::Timer rendererTickTimer;
        renderer->submitScene({0U, 0U});
      renderer->tick(forceResize);
        g_lastRendererTickTimeMs.store(
          static_cast<int32_t>(std::round(rendererTickTimer.elapsedMilliseconds())),
          std::memory_order_relaxed);
      eventDispatcher.dispatchQueued();

      uiLayer.endFrame();

      eventDispatcher.enqueue({
          .type = volt::event::EventType::kFrameEnded,
          .payload = volt::event::FrameLifecycleEvent{.frameIndex = frameIndex},
      });
      eventDispatcher.dispatchQueued();
      ++frameIndex;
      g_lastRenderFrameCpuTimeMs.store(
          static_cast<int32_t>(std::round(renderFrameCpuTimer.elapsedMilliseconds())),
          std::memory_order_relaxed);
      return requestFollowUpFrame;
    };

    window.setResizeRepaintCallback([&]() {
      if (callbackRepaintInProgress) {
        return;
      }

      callbackRepaintInProgress = true;
      renderDirty = renderFrameNow(false) || renderDirty;
      callbackRepaintInProgress = false;
    });

    while (!window.shouldClose()) {
      const volt::core::Timer waitTimer;
      const bool recentResizeActivityBeforeWait =
          std::chrono::steady_clock::now() < continuousRenderUntil;
      const bool shouldBlockForEvent =
          window.isMinimized() || (!renderDirty && !recentResizeActivityBeforeWait);

      leftMousePressedThisWake = false;

      if (shouldBlockForEvent) {
        window.waitEvents();
      } else {
        window.pollEvents();
      }
      g_lastEventWaitTimeMs.store(
          shouldBlockForEvent ? 0 : static_cast<int32_t>(std::round(waitTimer.elapsedMilliseconds())),
          std::memory_order_relaxed);
      if (window.shouldClose()) {
        break;
      }

      const volt::core::Timer frameTimer;

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

      const bool resizedThisWake = window.wasResized();
      if (resizedThisWake) {
        continuousRenderUntil = now + kResizeRenderHold;
      }

      const bool recentResizeActivity = (now < continuousRenderUntil);

      if (renderDirty || recentResizeActivity) {
        const bool requestFollowUpFrame = renderFrameNow(false);
        if (resizedThisWake) {
          window.acknowledgeResize();
        }
        renderDirty = requestFollowUpFrame || resizedThisWake;
      } else if (resizedThisWake) {
        window.acknowledgeResize();
      }

      VOLT_LOG_TRACE_CAT(
          volt::core::logging::Category::kApp,
          "Frame ",
          frameIndex,
          " logicTicks=",
          logicTicksThisWake,
          " durationMs=",
          frameTimer.elapsedMilliseconds());

      g_lastFrameTimeMs.store(static_cast<int32_t>(std::round(frameTimer.elapsedMilliseconds())), std::memory_order_relaxed);
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
