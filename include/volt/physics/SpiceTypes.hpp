#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace volt::physics::spice {

using NodeId = std::uint32_t;
inline constexpr NodeId kGroundNode = 0;

enum class ElementKind {
  kUnknown,
  kResistor,
  kCapacitor,
  kInductor,
  kVoltageSource,
  kCurrentSource,
  kDiode,
  kBjt,
  kMosfet,
  kSubcircuitInstance,
};

enum class AnalysisKind {
  kOperatingPoint,
  kDcSweep,
  kAcSweep,
  kTransient,
  kNoise,
};

enum class SweepScale {
  kLinear,
  kDecade,
  kOctave,
};

struct SweepRange {
  double start{0.0};
  double stop{0.0};
  double step{0.0};
  SweepScale scale{SweepScale::kLinear};
};

struct AnalysisRequest {
  AnalysisKind kind{AnalysisKind::kOperatingPoint};
  std::optional<SweepRange> primarySweep;
  std::optional<SweepRange> secondarySweep;
  std::optional<double> maxTimeSeconds;
  std::optional<double> timeStepSeconds;
};

using ParameterValue = std::variant<double, std::int64_t, bool, std::string>;

struct ParameterAssignment {
  std::string name;
  ParameterValue value{};
};

struct PinConnection {
  std::string pinName;
  NodeId node{kGroundNode};
};

}  // namespace volt::physics::spice
