#pragma once

#include <chrono>
#include <functional>

namespace volt::core {

class Timer {
 public:
  using Clock = std::chrono::steady_clock;

  Timer() : start_(Clock::now()) {}

  void reset() {
    start_ = Clock::now();
  }

  [[nodiscard]] double elapsedMilliseconds() const {
    return std::chrono::duration<double, std::milli>(Clock::now() - start_).count();
  }

  [[nodiscard]] double elapsedSeconds() const {
    return std::chrono::duration<double>(Clock::now() - start_).count();
  }

 private:
  Clock::time_point start_;
};

class ScopedTimer {
 public:
  using Callback = std::function<void(double)>;

  explicit ScopedTimer(Callback callback)
      : callback_(std::move(callback)) {}

  ~ScopedTimer() {
    if (callback_) {
      callback_(timer_.elapsedMilliseconds());
    }
  }

 private:
  Timer timer_;
  Callback callback_;
};

}  // namespace volt::core
