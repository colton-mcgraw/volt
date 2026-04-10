#pragma once

#include "volt/math/MathCommon.hpp"
#include "volt/math/Units.hpp"

#include <stdexcept>

namespace volt::physics::electrical {

[[nodiscard]] inline volt::math::Voltage voltageFromCurrentAndResistance(
    const volt::math::Current& current,
    const volt::math::Resistance& resistance,
    volt::math::VoltageUnit outputUnit = volt::math::VoltageUnit::kVolt) {
  const double volts = current.in(volt::math::CurrentUnit::kAmpere) * resistance.in(volt::math::ResistanceUnit::kOhm);
  return volt::math::makeVoltage(
      volt::math::convertVoltage(volts, volt::math::VoltageUnit::kVolt, outputUnit),
      outputUnit);
}

[[nodiscard]] inline volt::math::Current currentFromVoltageAndResistance(
    const volt::math::Voltage& voltage,
    const volt::math::Resistance& resistance,
    volt::math::CurrentUnit outputUnit = volt::math::CurrentUnit::kAmpere) {
  const double ohms = resistance.in(volt::math::ResistanceUnit::kOhm);
  if (volt::math::nearlyEqual(ohms, 0.0)) {
    throw std::invalid_argument("Resistance must be non-zero for I = V / R");
  }

  const double amperes = voltage.in(volt::math::VoltageUnit::kVolt) / ohms;
  return volt::math::makeCurrent(
      volt::math::convertCurrent(amperes, volt::math::CurrentUnit::kAmpere, outputUnit),
      outputUnit);
}

[[nodiscard]] inline volt::math::Resistance resistanceFromVoltageAndCurrent(
    const volt::math::Voltage& voltage,
    const volt::math::Current& current,
    volt::math::ResistanceUnit outputUnit = volt::math::ResistanceUnit::kOhm) {
  const double amperes = current.in(volt::math::CurrentUnit::kAmpere);
  if (volt::math::nearlyEqual(amperes, 0.0)) {
    throw std::invalid_argument("Current must be non-zero for R = V / I");
  }

  const double ohms = voltage.in(volt::math::VoltageUnit::kVolt) / amperes;
  return volt::math::makeResistance(
      volt::math::convertResistance(ohms, volt::math::ResistanceUnit::kOhm, outputUnit),
      outputUnit);
}

[[nodiscard]] inline volt::math::Power powerFromVoltageAndCurrent(
    const volt::math::Voltage& voltage,
    const volt::math::Current& current,
    volt::math::PowerUnit outputUnit = volt::math::PowerUnit::kWatt) {
  const double watts = voltage.in(volt::math::VoltageUnit::kVolt) * current.in(volt::math::CurrentUnit::kAmpere);
  return volt::math::makePower(
      volt::math::convertPower(watts, volt::math::PowerUnit::kWatt, outputUnit),
      outputUnit);
}

[[nodiscard]] inline volt::math::Power powerFromCurrentAndResistance(
    const volt::math::Current& current,
    const volt::math::Resistance& resistance,
    volt::math::PowerUnit outputUnit = volt::math::PowerUnit::kWatt) {
  const double amperes = current.in(volt::math::CurrentUnit::kAmpere);
  const double watts = (amperes * amperes) * resistance.in(volt::math::ResistanceUnit::kOhm);
  return volt::math::makePower(
      volt::math::convertPower(watts, volt::math::PowerUnit::kWatt, outputUnit),
      outputUnit);
}

[[nodiscard]] inline volt::math::Power powerFromVoltageAndResistance(
    const volt::math::Voltage& voltage,
    const volt::math::Resistance& resistance,
    volt::math::PowerUnit outputUnit = volt::math::PowerUnit::kWatt) {
  const double ohms = resistance.in(volt::math::ResistanceUnit::kOhm);
  if (volt::math::nearlyEqual(ohms, 0.0)) {
    throw std::invalid_argument("Resistance must be non-zero for P = V^2 / R");
  }

  const double volts = voltage.in(volt::math::VoltageUnit::kVolt);
  const double watts = (volts * volts) / ohms;
  return volt::math::makePower(
      volt::math::convertPower(watts, volt::math::PowerUnit::kWatt, outputUnit),
      outputUnit);
}

}  // namespace volt::physics::electrical
