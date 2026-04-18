#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace volt::event {
class EventDispatcher;
}

namespace volt::ui {
struct UiMeshData;
}

namespace volt::render {

class Renderer {
 public:
  struct SceneSubmission {
    std::uint32_t meshCount{0};
    std::uint32_t instanceCount{0};
  };

  struct VectorTextGpuTimings {
    bool valid{false};
    std::int32_t flattenCountMs{0};
    std::int32_t curveScanMs{0};
    std::int32_t flattenEmitBinCountMs{0};
    std::int32_t tileScanMs{0};
    std::int32_t binEmitFineMs{0};
  };

  struct FrameCpuTimings {
    bool valid{false};
    std::int32_t fenceWaitMs{0};
    std::int32_t acquireMs{0};
    std::int32_t recordMs{0};
    std::int32_t recordUploadMs{0};
    std::int32_t recordDrawMs{0};
    std::int32_t uiVectorTextPrepMs{0};
    std::int32_t submitMs{0};
    std::int32_t presentMs{0};
    std::int32_t uiDrawCallCount{0};
    double uiDrawCallAvgMs{0.0};
    double uiDrawCallMaxMs{0.0};
    std::int32_t uiDrawCallMaxBatchIndex{-1};
    std::string uiDrawCallMaxTextureKey;
    double uiDescriptorResolveTotalMs{0.0};
    double uiDescriptorResolveMaxMs{0.0};
    std::string uiDescriptorResolveMaxTextureKey;
  };

  using UiMeshProvider = std::function<const volt::ui::UiMeshData*()>;

  virtual ~Renderer() = default;

  virtual void submitScene(const SceneSubmission& submission) = 0;
  virtual void setUiMeshProvider(UiMeshProvider provider) = 0;
  virtual void setEventDispatcher(volt::event::EventDispatcher* dispatcher) = 0;
  virtual void tick(bool framebufferResized) = 0;
  [[nodiscard]] virtual VectorTextGpuTimings vectorTextGpuTimings() const = 0;
  [[nodiscard]] virtual FrameCpuTimings frameCpuTimings() const = 0;
};

}  // namespace volt::render
