#pragma once

#include "volt/ui/UIRenderTypes.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace volt::ui {

struct UiVertex {
  float x{0.0F};
  float y{0.0F};
  float u{0.0F};
  float v{0.0F};
  Color color{};
};

struct UiMeshBatch {
  std::uint64_t widgetId{0};
  std::uint32_t firstIndex{0};
  std::uint32_t indexCount{0};
  Rect clipRect{};
  std::string textureKey{"__white"};
};

struct UiMeshData {
  std::vector<UiVertex> vertices;
  std::vector<std::uint32_t> indices;
  std::vector<UiMeshBatch> batches;
};

[[nodiscard]] UiMeshData buildUiMesh(const std::vector<UiRenderCommand>& commands);

}  // namespace volt::ui
