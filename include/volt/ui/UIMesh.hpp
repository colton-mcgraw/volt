#pragma once

#include "volt/ui/UIRenderTypes.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace volt::ui {

struct UiVertex {
  float x{0.0F};
  float y{0.0F};
  float z{0.0F};
  float u{0.0F};
  float v{0.0F};
  Color color{};
};

enum class UiBatchLayer : std::uint8_t {
  kOpaque,
  kTransparent,
};

struct UiVectorCurveInput {
  float p0x{0.0F};
  float p0y{0.0F};
  float p1x{0.0F};
  float p1y{0.0F};
  float p2x{0.0F};
  float p2y{0.0F};
  float p3x{0.0F};
  float p3y{0.0F};
  std::uint32_t pathId{0};
  std::uint32_t isCubic{0};
};

struct UiVectorTextBatch {
  std::uint64_t widgetId{0};
  Rect bounds{};
  Rect clipRect{};
  std::string textureKey{};
  Color color{};
  std::uint32_t imageWidth{0};
  std::uint32_t imageHeight{0};
  std::vector<UiVectorCurveInput> curves{};
};

struct UiMeshBatch {
  std::uint64_t widgetId{0};
  std::uint32_t firstIndex{0};
  std::uint32_t indexCount{0};
  Rect clipRect{};
  std::string textureKey{"__white"};
  UiBatchLayer layer{UiBatchLayer::kTransparent};
  bool sdfText{false};
  bool msdfText{false};
  float sdfPxRange{0.0F};
  float sdfEdge{0.5F};
  float sdfAaStrength{0.35F};
  float msdfConfidenceLow{0.01F};
  float msdfConfidenceHigh{0.07F};
  float subpixelBlendStrength{0.85F};
  float smallTextSharpenStrength{0.28F};
};

struct UiRetainedPanel {
  std::uint64_t widgetId{0};
  Rect bounds{};
  Rect clipRect{};
  std::string textureKey{};
  std::uint64_t signature{0};
  std::vector<UiVertex> vertices{};
  std::vector<std::uint32_t> indices{};
  std::vector<UiMeshBatch> batches{};
};

// Key for batching compatibility (texture, text flags, and clip rect)
struct BatchKey {
  std::string textureKey;
  UiBatchLayer layer;
  bool sdfText;
  bool msdfText;
  Rect clipRect;

  bool operator==(const BatchKey& other) const {
    return textureKey == other.textureKey &&
           layer == other.layer &&
           sdfText == other.sdfText &&
           msdfText == other.msdfText &&
           clipRect.x == other.clipRect.x &&
           clipRect.y == other.clipRect.y &&
           clipRect.width == other.clipRect.width &&
           clipRect.height == other.clipRect.height;
  }
};

struct UiMeshData {
  std::vector<UiVertex> vertices;
  std::vector<std::uint32_t> indices;
  std::vector<UiMeshBatch> batches;
  std::vector<UiVectorTextBatch> vectorTextBatches;
  std::vector<UiRetainedPanel> retainedPanels;
};

struct UiMeshBuildOptions {
  bool enableOpaqueBatching{false};
};

[[nodiscard]] UiMeshData buildUiMesh(
    const std::vector<UiRenderCommand>& commands,
    UiMeshBuildOptions options = {});

}  // namespace volt::ui
