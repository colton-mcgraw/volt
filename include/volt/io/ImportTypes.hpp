#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace volt::io {

enum class ModelFormat {
  kStep,
  kThreeMf,
  kUnknown
};

enum class IssueSeverity {
  kInfo,
  kWarning,
  kError
};

struct ImportIssue {
  IssueSeverity severity{IssueSeverity::kInfo};
  std::string code;
  std::string message;
};

struct Vec3 {
  float x{0.0F};
  float y{0.0F};
  float z{0.0F};
};

struct MeshData {
  std::string name;
  std::vector<Vec3> positions;
  std::vector<std::uint32_t> triangleIndices;
};

struct SceneNode {
  std::string name;
  std::vector<MeshData> meshes;
};

struct ImportedScene {
  std::vector<SceneNode> nodes;
};

struct ImportOptions {
  bool triangulate{true};
  bool mergeDuplicateVertices{true};
  float unitScale{1.0F};
};

struct ImportRequest {
  std::filesystem::path path;
  ModelFormat formatHint{ModelFormat::kUnknown};
  ImportOptions options{};
};

struct ImportResult {
  bool success{false};
  std::string message;
  ModelFormat detectedFormat{ModelFormat::kUnknown};
  ImportedScene scene;
  std::vector<ImportIssue> issues;
};

}  // namespace volt::io
