#pragma once

#include "volt/physics/SpiceTypes.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace volt::physics::spice {

struct ModelParameterDefinition {
  std::string name;
  ParameterValue defaultValue{0.0};
  bool required{false};
  std::string description;
};

struct DeviceModelDefinition {
  std::string name;
  ElementKind elementKind{ElementKind::kUnknown};
  std::string level;
  std::vector<std::string> terminalOrder;
  std::vector<ModelParameterDefinition> parameters;
};

class ModelLibrary {
 public:
  void registerDefinition(const DeviceModelDefinition& definition) {
    definitions_[definition.name] = definition;
  }

  [[nodiscard]] bool hasDefinition(const std::string& name) const {
    return definitions_.find(name) != definitions_.end();
  }

  [[nodiscard]] std::optional<DeviceModelDefinition> findDefinition(const std::string& name) const {
    const auto it = definitions_.find(name);
    if (it == definitions_.end()) {
      return std::nullopt;
    }
    return it->second;
  }

 private:
  std::unordered_map<std::string, DeviceModelDefinition> definitions_;
};

}  // namespace volt::physics::spice
