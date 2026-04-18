#pragma once

#include <algorithm>
#include <cmath>

namespace volt::platform::details {

struct StartupClientSize {
  int logicalWidth{1};
  int logicalHeight{1};
  int physicalWidth{1};
  int physicalHeight{1};
};

[[nodiscard]] inline StartupClientSize resolveStartupClientSize(
    int requestedLogicalWidth,
    int requestedLogicalHeight,
    int workAreaPhysicalWidth,
    int workAreaPhysicalHeight,
    float contentScaleX,
    float contentScaleY) {
  const float safeScaleX = std::max(0.01F, contentScaleX);
  const float safeScaleY = std::max(0.01F, contentScaleY);
  const int logicalWorkWidth =
      std::max(1, static_cast<int>(std::lround(static_cast<double>(std::max(1, workAreaPhysicalWidth)) / safeScaleX)));
  const int logicalWorkHeight =
      std::max(1, static_cast<int>(std::lround(static_cast<double>(std::max(1, workAreaPhysicalHeight)) / safeScaleY)));

  StartupClientSize result{};
  result.logicalWidth = requestedLogicalWidth > 0 ? requestedLogicalWidth : std::max(1, logicalWorkWidth / 2);
  result.logicalHeight = requestedLogicalHeight > 0 ? requestedLogicalHeight : std::max(1, logicalWorkHeight / 2);
  result.physicalWidth =
      std::max(1, static_cast<int>(std::lround(static_cast<double>(result.logicalWidth) * safeScaleX)));
  result.physicalHeight =
      std::max(1, static_cast<int>(std::lround(static_cast<double>(result.logicalHeight) * safeScaleY)));
  return result;
}

}  // namespace volt::platform::details