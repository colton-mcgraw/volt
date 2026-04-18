#include "volt/render/details/VectorTextWorker.hpp"

namespace volt::render::details {

VectorTextWorker::VectorTextWorker() {
  isRunning_ = true;
  workerThread_ = std::thread([this]() { workerThreadMain(); });
}

VectorTextWorker::~VectorTextWorker() {
  shutdown();
}

void VectorTextWorker::enqueueBatch(const VectorTextPrepRequest& request) {
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    requestQueue_.push(request);
  }
  queueCV_.notify_one();
}

std::optional<PreparedVectorTextBatch> VectorTextWorker::tryGetPreparedBatch() {
  std::lock_guard<std::mutex> lock(queueMutex_);
  if (preparedBatches_.empty()) {
    return std::nullopt;
  }
  
  PreparedVectorTextBatch result = std::move(preparedBatches_.back());
  preparedBatches_.pop_back();
  return result;
}

void VectorTextWorker::shutdown() {
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    shouldShutdown_ = true;
  }
  queueCV_.notify_one();
  
  if (workerThread_.joinable()) {
    workerThread_.join();
  }
  
  isRunning_ = false;
}

bool VectorTextWorker::isRunning() const {
  return isRunning_;
}

void VectorTextWorker::workerThreadMain() {
  while (true) {
    VectorTextPrepRequest request;
    
    {
      std::unique_lock<std::mutex> lock(queueMutex_);
      queueCV_.wait(lock, [this]() {
        return !requestQueue_.empty() || shouldShutdown_;
      });
      
      if (shouldShutdown_ && requestQueue_.empty()) {
        break;
      }
      
      if (requestQueue_.empty()) {
        continue;
      }
      
      request = requestQueue_.front();
      requestQueue_.pop();
    }

    // Process the batch (data is already prepared by main thread, just transfer)
    PreparedVectorTextBatch prepared{
        .textureKey = request.textureKey,
        .imageWidth = request.imageWidth,
        .imageHeight = request.imageHeight,
        .color = request.color,
        .curves = std::move(request.curves),
    };

    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      preparedBatches_.push_back(std::move(prepared));
    }
  }
}

}  // namespace volt::render::details
