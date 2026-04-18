#pragma once

#include "volt/ui/UIMesh.hpp"
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <memory>
#include <optional>

namespace volt::render::details {

// Prepared vector text batch ready for GPU rendering
struct PreparedVectorTextBatch {
  std::string textureKey;
  std::uint32_t imageWidth;
  std::uint32_t imageHeight;
  volt::ui::Color color;
  std::vector<volt::ui::UiVectorCurveInput> curves;
};

// Work request for the prep thread
struct VectorTextPrepRequest {
  std::string textureKey;
  std::vector<volt::ui::UiVectorCurveInput> curves;
  volt::ui::Color color;
  std::uint32_t imageWidth;
  std::uint32_t imageHeight;
};

class VectorTextWorker {
 public:
  VectorTextWorker();
  ~VectorTextWorker();

  VectorTextWorker(const VectorTextWorker&) = delete;
  VectorTextWorker& operator=(const VectorTextWorker&) = delete;

  // Queue a batch for preparation
  void enqueueBatch(const VectorTextPrepRequest& request);

  // Get a prepared batch if available (non-blocking)
  std::optional<PreparedVectorTextBatch> tryGetPreparedBatch();

  // Shutdown the worker thread gracefully
  void shutdown();

  // Check if worker thread is still running
  [[nodiscard]] bool isRunning() const;

 private:
  void workerThreadMain();

  std::vector<PreparedVectorTextBatch> preparedBatches_;
  std::queue<VectorTextPrepRequest> requestQueue_;
  std::mutex queueMutex_;
  std::condition_variable queueCV_;
  std::thread workerThread_;
  bool shouldShutdown_{false};
  bool isRunning_{false};
};

}  // namespace volt::render::details
