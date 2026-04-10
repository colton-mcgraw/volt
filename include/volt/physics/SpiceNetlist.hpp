#pragma once

#include "volt/physics/SpiceTypes.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace volt::physics::spice {

struct ModelReference {
  std::string modelName;
  std::string level;
};

struct ElementInstance {
  std::string name;
  ElementKind kind{ElementKind::kUnknown};
  std::vector<PinConnection> pins;
  std::vector<ParameterAssignment> parameters;
  std::optional<ModelReference> model;
};

struct SubcircuitDefinition {
  std::string name;
  std::vector<std::string> externalPins;
  std::vector<ElementInstance> elements;
  std::vector<ParameterAssignment> defaultParameters;
};

struct Netlist {
  std::string title;
  std::unordered_map<std::string, NodeId> nodeAliases;
  std::vector<ElementInstance> topLevelElements;
  std::vector<SubcircuitDefinition> subcircuits;
  std::vector<ParameterAssignment> globalParameters;
  std::vector<AnalysisRequest> analyses;
};

}  // namespace volt::physics::spice
