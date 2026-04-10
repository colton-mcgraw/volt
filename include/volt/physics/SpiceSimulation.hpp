#pragma once

#include "volt/physics/SpiceNetlist.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace volt::physics::spice {

enum class IntegrationMethod {
  kBackwardEuler,
  kTrapezoidal,
  kGear2,
};

struct NonLinearSolveOptions {
  std::size_t maxIterations{100};
  double absTolerance{1e-12};
  double relTolerance{1e-6};
  double voltageTolerance{1e-6};
  double currentTolerance{1e-12};
};

struct SimulationOptions {
  IntegrationMethod integration{IntegrationMethod::kTrapezoidal};
  NonLinearSolveOptions nonLinear;
  bool gminSteppingEnabled{true};
  bool sourceSteppingEnabled{true};
};

struct CompiledNetlistPlan {
  std::size_t nodeCount{0};
  std::size_t branchEquationCount{0};
  std::vector<std::string> unknownLabels;
};

struct SamplePoint {
  double timeSeconds{0.0};
  std::vector<double> nodeVoltages;
  std::vector<double> branchCurrents;
};

struct AnalysisResult {
  AnalysisKind kind{AnalysisKind::kOperatingPoint};
  bool converged{false};
  std::string message;
  std::vector<SamplePoint> samples;
};

class ISimulationEngine {
 public:
  virtual ~ISimulationEngine() = default;

  virtual CompiledNetlistPlan compile(const Netlist& netlist, const SimulationOptions& options) = 0;
  virtual AnalysisResult run(
      const Netlist& netlist,
      const AnalysisRequest& request,
      const SimulationOptions& options) = 0;
};

}  // namespace volt::physics::spice
