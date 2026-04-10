#pragma once

#include "volt/math/Units.hpp"
#include "volt/physics/SpiceNetlist.hpp"

#include <cstddef>
#include <string>

namespace volt::physics::spice {

struct DeviceEvaluationContext {
  AnalysisKind analysisKind{AnalysisKind::kOperatingPoint};
  double simulationTimeSeconds{0.0};
  double stepSeconds{0.0};
  volt::math::Temperature temperature{300.15, volt::math::TemperatureUnit::kKelvin};
};

class MnaSystemView {
 public:
  virtual ~MnaSystemView() = default;

  virtual void stampConductance(NodeId from, NodeId to, double siemens) = 0;
  virtual void stampCurrentSource(NodeId from, NodeId to, double amperes) = 0;
  virtual std::size_t allocateBranchEquation(const std::string& label) = 0;
  virtual void stampVoltageSource(
      NodeId positive,
      NodeId negative,
      std::size_t branchEquation,
      double volts) = 0;
};

class IDeviceStamper {
 public:
  virtual ~IDeviceStamper() = default;

  virtual void stampLinear(
      const ElementInstance& element,
      const DeviceEvaluationContext& context,
      MnaSystemView& system) const = 0;

  virtual void stampNonLinear(
      const ElementInstance& element,
      const DeviceEvaluationContext& context,
      MnaSystemView& system) const {
    (void)element;
    (void)context;
    (void)system;
  }
};

}  // namespace volt::physics::spice
