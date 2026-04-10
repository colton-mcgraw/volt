#pragma once

namespace volt::math {

enum class LengthUnit {
  kMillimeter,
  kCentimeter,
  kMeter,
  kKilometer,
  kInch,
  kFoot,
  kYard,
  kMile,
  kUsSurveyFoot,
};

enum class AngleUnit {
  kRadian,
  kDegree,
};

enum class MassUnit {
  kKilogram,
  kGram,
  kPound,
  kOunce,
};

enum class TimeUnit {
  kSecond,
  kMillisecond,
  kMinute,
  kHour,
};

enum class AreaUnit {
  kSquareMeter,
  kSquareFoot,
  kSquareInch,
  kAcre,
  kHectare,
};

enum class VolumeUnit {
  kCubicMeter,
  kLiter,
  kMilliliter,
  kCubicFoot,
  kUsGallon,
  kImperialGallon,
};

enum class TemperatureUnit {
  kKelvin,
  kCelsius,
  kFahrenheit,
};

enum class VoltageUnit {
  kMicrovolt,
  kMillivolt,
  kVolt,
  kKilovolt,
};

enum class CurrentUnit {
  kMicroampere,
  kMilliampere,
  kAmpere,
  kKiloampere,
};

enum class ResistanceUnit {
  kMilliohm,
  kOhm,
  kKiloohm,
  kMegaohm,
};

enum class PowerUnit {
  kMicrowatt,
  kMilliwatt,
  kWatt,
  kKilowatt,
  kMegawatt,
};

enum class CapacitanceUnit {
  kPicofarad,
  kNanofarad,
  kMicrofarad,
  kMillifarad,
  kFarad,
};

enum class InductanceUnit {
  kNanohenry,
  kMicrohenry,
  kMillihenry,
  kHenry,
};

enum class MagneticFluxDensityUnit {
  kTesla,
  kMillitesla,
  kMicrotesla,
  kGauss,
};

enum class MagneticFluxUnit {
  kWeber,
  kMilliweber,
  kMaxwell,
};

enum class FrequencyUnit {
  kHertz,
  kKilohertz,
  kMegahertz,
  kGigahertz,
};

enum class ChargeUnit {
  kCoulomb,
  kMilliampHour,
  kAmpHour,
};

enum class ConductanceUnit {
  kSiemens,
  kMillisiemens,
  kMicrosiemens,
};

enum class EnergyUnit {
  kJoule,
  kWattHour,
  kKilowattHour,
  kElectronvolt,
};

enum class ForceUnit {
  kNewton,
  kKilonewton,
  kPoundForce,
};

enum class PressureUnit {
  kPascal,
  kKilopascal,
  kBar,
  kPsi,
};

[[nodiscard]] constexpr double lengthFactorToMeter(LengthUnit unit) {
  switch (unit) {
    case LengthUnit::kMillimeter:
      return 0.001;
    case LengthUnit::kCentimeter:
      return 0.01;
    case LengthUnit::kMeter:
      return 1.0;
    case LengthUnit::kKilometer:
      return 1000.0;
    case LengthUnit::kInch:
      return 0.0254;
    case LengthUnit::kFoot:
      return 0.3048;
    case LengthUnit::kYard:
      return 0.9144;
    case LengthUnit::kMile:
      return 1609.344;
    case LengthUnit::kUsSurveyFoot:
      return 1200.0 / 3937.0;
    default:
      return 1.0;
  }
}

[[nodiscard]] constexpr double angleFactorToRadian(AngleUnit unit) {
  switch (unit) {
    case AngleUnit::kRadian:
      return 1.0;
    case AngleUnit::kDegree:
      return 0.017453292519943295;
    default:
      return 1.0;
  }
}

[[nodiscard]] constexpr double massFactorToKilogram(MassUnit unit) {
  switch (unit) {
    case MassUnit::kKilogram:
      return 1.0;
    case MassUnit::kGram:
      return 0.001;
    case MassUnit::kPound:
      return 0.45359237;
    case MassUnit::kOunce:
      return 0.028349523125;
    default:
      return 1.0;
  }
}

[[nodiscard]] constexpr double timeFactorToSecond(TimeUnit unit) {
  switch (unit) {
    case TimeUnit::kSecond:
      return 1.0;
    case TimeUnit::kMillisecond:
      return 0.001;
    case TimeUnit::kMinute:
      return 60.0;
    case TimeUnit::kHour:
      return 3600.0;
    default:
      return 1.0;
  }
}

[[nodiscard]] constexpr double areaFactorToSquareMeter(AreaUnit unit) {
  switch (unit) {
    case AreaUnit::kSquareMeter:
      return 1.0;
    case AreaUnit::kSquareFoot:
      return 0.09290304;
    case AreaUnit::kSquareInch:
      return 0.00064516;
    case AreaUnit::kAcre:
      return 4046.8564224;
    case AreaUnit::kHectare:
      return 10000.0;
    default:
      return 1.0;
  }
}

[[nodiscard]] constexpr double volumeFactorToCubicMeter(VolumeUnit unit) {
  switch (unit) {
    case VolumeUnit::kCubicMeter:
      return 1.0;
    case VolumeUnit::kLiter:
      return 0.001;
    case VolumeUnit::kMilliliter:
      return 0.000001;
    case VolumeUnit::kCubicFoot:
      return 0.028316846592;
    case VolumeUnit::kUsGallon:
      return 0.003785411784;
    case VolumeUnit::kImperialGallon:
      return 0.00454609;
    default:
      return 1.0;
  }
}

[[nodiscard]] constexpr double voltageFactorToVolt(VoltageUnit unit) {
  switch (unit) {
    case VoltageUnit::kMicrovolt:
      return 0.000001;
    case VoltageUnit::kMillivolt:
      return 0.001;
    case VoltageUnit::kVolt:
      return 1.0;
    case VoltageUnit::kKilovolt:
      return 1000.0;
    default:
      return 1.0;
  }
}

[[nodiscard]] constexpr double currentFactorToAmpere(CurrentUnit unit) {
  switch (unit) {
    case CurrentUnit::kMicroampere:
      return 0.000001;
    case CurrentUnit::kMilliampere:
      return 0.001;
    case CurrentUnit::kAmpere:
      return 1.0;
    case CurrentUnit::kKiloampere:
      return 1000.0;
    default:
      return 1.0;
  }
}

[[nodiscard]] constexpr double resistanceFactorToOhm(ResistanceUnit unit) {
  switch (unit) {
    case ResistanceUnit::kMilliohm:
      return 0.001;
    case ResistanceUnit::kOhm:
      return 1.0;
    case ResistanceUnit::kKiloohm:
      return 1000.0;
    case ResistanceUnit::kMegaohm:
      return 1000000.0;
    default:
      return 1.0;
  }
}

[[nodiscard]] constexpr double powerFactorToWatt(PowerUnit unit) {
  switch (unit) {
    case PowerUnit::kMicrowatt:
      return 0.000001;
    case PowerUnit::kMilliwatt:
      return 0.001;
    case PowerUnit::kWatt:
      return 1.0;
    case PowerUnit::kKilowatt:
      return 1000.0;
    case PowerUnit::kMegawatt:
      return 1000000.0;
    default:
      return 1.0;
  }
}

[[nodiscard]] constexpr double capacitanceFactorToFarad(CapacitanceUnit unit) {
  switch (unit) {
    case CapacitanceUnit::kPicofarad:
      return 1e-12;
    case CapacitanceUnit::kNanofarad:
      return 1e-9;
    case CapacitanceUnit::kMicrofarad:
      return 1e-6;
    case CapacitanceUnit::kMillifarad:
      return 1e-3;
    case CapacitanceUnit::kFarad:
      return 1.0;
    default:
      return 1.0;
  }
}

[[nodiscard]] constexpr double inductanceFactorToHenry(InductanceUnit unit) {
  switch (unit) {
    case InductanceUnit::kNanohenry:
      return 1e-9;
    case InductanceUnit::kMicrohenry:
      return 1e-6;
    case InductanceUnit::kMillihenry:
      return 1e-3;
    case InductanceUnit::kHenry:
      return 1.0;
    default:
      return 1.0;
  }
}

[[nodiscard]] constexpr double magneticFluxDensityFactorToTesla(MagneticFluxDensityUnit unit) {
  switch (unit) {
    case MagneticFluxDensityUnit::kTesla:
      return 1.0;
    case MagneticFluxDensityUnit::kMillitesla:
      return 1e-3;
    case MagneticFluxDensityUnit::kMicrotesla:
      return 1e-6;
    case MagneticFluxDensityUnit::kGauss:
      return 1e-4;
    default:
      return 1.0;
  }
}

[[nodiscard]] constexpr double magneticFluxFactorToWeber(MagneticFluxUnit unit) {
  switch (unit) {
    case MagneticFluxUnit::kWeber:
      return 1.0;
    case MagneticFluxUnit::kMilliweber:
      return 1e-3;
    case MagneticFluxUnit::kMaxwell:
      return 1e-8;
    default:
      return 1.0;
  }
}

[[nodiscard]] constexpr double frequencyFactorToHertz(FrequencyUnit unit) {
  switch (unit) {
    case FrequencyUnit::kHertz:
      return 1.0;
    case FrequencyUnit::kKilohertz:
      return 1000.0;
    case FrequencyUnit::kMegahertz:
      return 1000000.0;
    case FrequencyUnit::kGigahertz:
      return 1000000000.0;
    default:
      return 1.0;
  }
}

[[nodiscard]] constexpr double chargeFactorToCoulomb(ChargeUnit unit) {
  switch (unit) {
    case ChargeUnit::kCoulomb:
      return 1.0;
    case ChargeUnit::kMilliampHour:
      return 3.6;
    case ChargeUnit::kAmpHour:
      return 3600.0;
    default:
      return 1.0;
  }
}

[[nodiscard]] constexpr double conductanceFactorToSiemens(ConductanceUnit unit) {
  switch (unit) {
    case ConductanceUnit::kSiemens:
      return 1.0;
    case ConductanceUnit::kMillisiemens:
      return 1e-3;
    case ConductanceUnit::kMicrosiemens:
      return 1e-6;
    default:
      return 1.0;
  }
}

[[nodiscard]] constexpr double energyFactorToJoule(EnergyUnit unit) {
  switch (unit) {
    case EnergyUnit::kJoule:
      return 1.0;
    case EnergyUnit::kWattHour:
      return 3600.0;
    case EnergyUnit::kKilowattHour:
      return 3600000.0;
    case EnergyUnit::kElectronvolt:
      return 1.602176634e-19;
    default:
      return 1.0;
  }
}

[[nodiscard]] constexpr double forceFactorToNewton(ForceUnit unit) {
  switch (unit) {
    case ForceUnit::kNewton:
      return 1.0;
    case ForceUnit::kKilonewton:
      return 1000.0;
    case ForceUnit::kPoundForce:
      return 4.4482216152605;
    default:
      return 1.0;
  }
}

[[nodiscard]] constexpr double pressureFactorToPascal(PressureUnit unit) {
  switch (unit) {
    case PressureUnit::kPascal:
      return 1.0;
    case PressureUnit::kKilopascal:
      return 1000.0;
    case PressureUnit::kBar:
      return 100000.0;
    case PressureUnit::kPsi:
      return 6894.757293168;
    default:
      return 1.0;
  }
}

[[nodiscard]] constexpr double toKelvin(double value, TemperatureUnit unit) {
  switch (unit) {
    case TemperatureUnit::kKelvin:
      return value;
    case TemperatureUnit::kCelsius:
      return value + 273.15;
    case TemperatureUnit::kFahrenheit:
      return (value - 32.0) * (5.0 / 9.0) + 273.15;
    default:
      return value;
  }
}

[[nodiscard]] constexpr double fromKelvin(double kelvin, TemperatureUnit unit) {
  switch (unit) {
    case TemperatureUnit::kKelvin:
      return kelvin;
    case TemperatureUnit::kCelsius:
      return kelvin - 273.15;
    case TemperatureUnit::kFahrenheit:
      return (kelvin - 273.15) * (9.0 / 5.0) + 32.0;
    default:
      return kelvin;
  }
}

[[nodiscard]] constexpr double convertLength(double value, LengthUnit from, LengthUnit to) {
  return value * lengthFactorToMeter(from) / lengthFactorToMeter(to);
}

[[nodiscard]] constexpr double convertAngle(double value, AngleUnit from, AngleUnit to) {
  return value * angleFactorToRadian(from) / angleFactorToRadian(to);
}

[[nodiscard]] constexpr double convertMass(double value, MassUnit from, MassUnit to) {
  return value * massFactorToKilogram(from) / massFactorToKilogram(to);
}

[[nodiscard]] constexpr double convertTime(double value, TimeUnit from, TimeUnit to) {
  return value * timeFactorToSecond(from) / timeFactorToSecond(to);
}

[[nodiscard]] constexpr double convertArea(double value, AreaUnit from, AreaUnit to) {
  return value * areaFactorToSquareMeter(from) / areaFactorToSquareMeter(to);
}

[[nodiscard]] constexpr double convertVolume(double value, VolumeUnit from, VolumeUnit to) {
  return value * volumeFactorToCubicMeter(from) / volumeFactorToCubicMeter(to);
}

[[nodiscard]] constexpr double convertTemperature(double value, TemperatureUnit from, TemperatureUnit to) {
  return fromKelvin(toKelvin(value, from), to);
}

[[nodiscard]] constexpr double convertVoltage(double value, VoltageUnit from, VoltageUnit to) {
  return value * voltageFactorToVolt(from) / voltageFactorToVolt(to);
}

[[nodiscard]] constexpr double convertCurrent(double value, CurrentUnit from, CurrentUnit to) {
  return value * currentFactorToAmpere(from) / currentFactorToAmpere(to);
}

[[nodiscard]] constexpr double convertResistance(double value, ResistanceUnit from, ResistanceUnit to) {
  return value * resistanceFactorToOhm(from) / resistanceFactorToOhm(to);
}

[[nodiscard]] constexpr double convertPower(double value, PowerUnit from, PowerUnit to) {
  return value * powerFactorToWatt(from) / powerFactorToWatt(to);
}

[[nodiscard]] constexpr double convertCapacitance(double value, CapacitanceUnit from, CapacitanceUnit to) {
  return value * capacitanceFactorToFarad(from) / capacitanceFactorToFarad(to);
}

[[nodiscard]] constexpr double convertInductance(double value, InductanceUnit from, InductanceUnit to) {
  return value * inductanceFactorToHenry(from) / inductanceFactorToHenry(to);
}

[[nodiscard]] constexpr double convertMagneticFluxDensity(
    double value,
    MagneticFluxDensityUnit from,
    MagneticFluxDensityUnit to) {
  return value * magneticFluxDensityFactorToTesla(from) / magneticFluxDensityFactorToTesla(to);
}

[[nodiscard]] constexpr double convertMagneticFlux(double value, MagneticFluxUnit from, MagneticFluxUnit to) {
  return value * magneticFluxFactorToWeber(from) / magneticFluxFactorToWeber(to);
}

[[nodiscard]] constexpr double convertFrequency(double value, FrequencyUnit from, FrequencyUnit to) {
  return value * frequencyFactorToHertz(from) / frequencyFactorToHertz(to);
}

[[nodiscard]] constexpr double convertCharge(double value, ChargeUnit from, ChargeUnit to) {
  return value * chargeFactorToCoulomb(from) / chargeFactorToCoulomb(to);
}

[[nodiscard]] constexpr double convertConductance(double value, ConductanceUnit from, ConductanceUnit to) {
  return value * conductanceFactorToSiemens(from) / conductanceFactorToSiemens(to);
}

[[nodiscard]] constexpr double convertEnergy(double value, EnergyUnit from, EnergyUnit to) {
  return value * energyFactorToJoule(from) / energyFactorToJoule(to);
}

[[nodiscard]] constexpr double convertForce(double value, ForceUnit from, ForceUnit to) {
  return value * forceFactorToNewton(from) / forceFactorToNewton(to);
}

[[nodiscard]] constexpr double convertPressure(double value, PressureUnit from, PressureUnit to) {
  return value * pressureFactorToPascal(from) / pressureFactorToPascal(to);
}

template <typename UnitEnum>
struct UnitTraits;

template <>
struct UnitTraits<LengthUnit> {
  [[nodiscard]] static constexpr double toBase(LengthUnit unit) {
    return lengthFactorToMeter(unit);
  }
};

template <>
struct UnitTraits<AngleUnit> {
  [[nodiscard]] static constexpr double toBase(AngleUnit unit) {
    return angleFactorToRadian(unit);
  }
};

template <>
struct UnitTraits<MassUnit> {
  [[nodiscard]] static constexpr double toBase(MassUnit unit) {
    return massFactorToKilogram(unit);
  }
};

template <>
struct UnitTraits<TimeUnit> {
  [[nodiscard]] static constexpr double toBase(TimeUnit unit) {
    return timeFactorToSecond(unit);
  }
};

template <>
struct UnitTraits<AreaUnit> {
  [[nodiscard]] static constexpr double toBase(AreaUnit unit) {
    return areaFactorToSquareMeter(unit);
  }
};

template <>
struct UnitTraits<VolumeUnit> {
  [[nodiscard]] static constexpr double toBase(VolumeUnit unit) {
    return volumeFactorToCubicMeter(unit);
  }
};

template <>
struct UnitTraits<VoltageUnit> {
  [[nodiscard]] static constexpr double toBase(VoltageUnit unit) {
    return voltageFactorToVolt(unit);
  }
};

template <>
struct UnitTraits<CurrentUnit> {
  [[nodiscard]] static constexpr double toBase(CurrentUnit unit) {
    return currentFactorToAmpere(unit);
  }
};

template <>
struct UnitTraits<ResistanceUnit> {
  [[nodiscard]] static constexpr double toBase(ResistanceUnit unit) {
    return resistanceFactorToOhm(unit);
  }
};

template <>
struct UnitTraits<PowerUnit> {
  [[nodiscard]] static constexpr double toBase(PowerUnit unit) {
    return powerFactorToWatt(unit);
  }
};

template <>
struct UnitTraits<CapacitanceUnit> {
  [[nodiscard]] static constexpr double toBase(CapacitanceUnit unit) {
    return capacitanceFactorToFarad(unit);
  }
};

template <>
struct UnitTraits<InductanceUnit> {
  [[nodiscard]] static constexpr double toBase(InductanceUnit unit) {
    return inductanceFactorToHenry(unit);
  }
};

template <>
struct UnitTraits<MagneticFluxDensityUnit> {
  [[nodiscard]] static constexpr double toBase(MagneticFluxDensityUnit unit) {
    return magneticFluxDensityFactorToTesla(unit);
  }
};

template <>
struct UnitTraits<MagneticFluxUnit> {
  [[nodiscard]] static constexpr double toBase(MagneticFluxUnit unit) {
    return magneticFluxFactorToWeber(unit);
  }
};

template <>
struct UnitTraits<FrequencyUnit> {
  [[nodiscard]] static constexpr double toBase(FrequencyUnit unit) {
    return frequencyFactorToHertz(unit);
  }
};

template <>
struct UnitTraits<ChargeUnit> {
  [[nodiscard]] static constexpr double toBase(ChargeUnit unit) {
    return chargeFactorToCoulomb(unit);
  }
};

template <>
struct UnitTraits<ConductanceUnit> {
  [[nodiscard]] static constexpr double toBase(ConductanceUnit unit) {
    return conductanceFactorToSiemens(unit);
  }
};

template <>
struct UnitTraits<EnergyUnit> {
  [[nodiscard]] static constexpr double toBase(EnergyUnit unit) {
    return energyFactorToJoule(unit);
  }
};

template <>
struct UnitTraits<ForceUnit> {
  [[nodiscard]] static constexpr double toBase(ForceUnit unit) {
    return forceFactorToNewton(unit);
  }
};

template <>
struct UnitTraits<PressureUnit> {
  [[nodiscard]] static constexpr double toBase(PressureUnit unit) {
    return pressureFactorToPascal(unit);
  }
};

template <typename UnitEnum>
struct Quantity {
  double value{0.0};
  UnitEnum unit{};

  [[nodiscard]] constexpr double in(UnitEnum targetUnit) const {
    return value * UnitTraits<UnitEnum>::toBase(unit) / UnitTraits<UnitEnum>::toBase(targetUnit);
  }

  [[nodiscard]] constexpr Quantity<UnitEnum> as(UnitEnum targetUnit) const {
    return {in(targetUnit), targetUnit};
  }
};

struct Temperature {
  double value{0.0};
  TemperatureUnit unit{TemperatureUnit::kKelvin};

  [[nodiscard]] constexpr double in(TemperatureUnit targetUnit) const {
    return convertTemperature(value, unit, targetUnit);
  }

  [[nodiscard]] constexpr Temperature as(TemperatureUnit targetUnit) const {
    return {in(targetUnit), targetUnit};
  }
};

using Length = Quantity<LengthUnit>;
using Angle = Quantity<AngleUnit>;
using Mass = Quantity<MassUnit>;
using Duration = Quantity<TimeUnit>;
using Area = Quantity<AreaUnit>;
using Volume = Quantity<VolumeUnit>;
using Voltage = Quantity<VoltageUnit>;
using Current = Quantity<CurrentUnit>;
using Resistance = Quantity<ResistanceUnit>;
using Power = Quantity<PowerUnit>;
using Capacitance = Quantity<CapacitanceUnit>;
using Inductance = Quantity<InductanceUnit>;
using MagneticFluxDensity = Quantity<MagneticFluxDensityUnit>;
using MagneticFlux = Quantity<MagneticFluxUnit>;
using Frequency = Quantity<FrequencyUnit>;
using Charge = Quantity<ChargeUnit>;
using Conductance = Quantity<ConductanceUnit>;
using Energy = Quantity<EnergyUnit>;
using Force = Quantity<ForceUnit>;
using Pressure = Quantity<PressureUnit>;

[[nodiscard]] constexpr Length makeLength(double value, LengthUnit unit) {
  return {value, unit};
}

[[nodiscard]] constexpr Angle makeAngle(double value, AngleUnit unit) {
  return {value, unit};
}

[[nodiscard]] constexpr Mass makeMass(double value, MassUnit unit) {
  return {value, unit};
}

[[nodiscard]] constexpr Duration makeDuration(double value, TimeUnit unit) {
  return {value, unit};
}

[[nodiscard]] constexpr Area makeArea(double value, AreaUnit unit) {
  return {value, unit};
}

[[nodiscard]] constexpr Volume makeVolume(double value, VolumeUnit unit) {
  return {value, unit};
}

[[nodiscard]] constexpr Temperature makeTemperature(double value, TemperatureUnit unit) {
  return {value, unit};
}

[[nodiscard]] constexpr Voltage makeVoltage(double value, VoltageUnit unit) {
  return {value, unit};
}

[[nodiscard]] constexpr Current makeCurrent(double value, CurrentUnit unit) {
  return {value, unit};
}

[[nodiscard]] constexpr Resistance makeResistance(double value, ResistanceUnit unit) {
  return {value, unit};
}

[[nodiscard]] constexpr Power makePower(double value, PowerUnit unit) {
  return {value, unit};
}

[[nodiscard]] constexpr Capacitance makeCapacitance(double value, CapacitanceUnit unit) {
  return {value, unit};
}

[[nodiscard]] constexpr Inductance makeInductance(double value, InductanceUnit unit) {
  return {value, unit};
}

[[nodiscard]] constexpr MagneticFluxDensity makeMagneticFluxDensity(
    double value,
    MagneticFluxDensityUnit unit) {
  return {value, unit};
}

[[nodiscard]] constexpr MagneticFlux makeMagneticFlux(double value, MagneticFluxUnit unit) {
  return {value, unit};
}

[[nodiscard]] constexpr Frequency makeFrequency(double value, FrequencyUnit unit) {
  return {value, unit};
}

[[nodiscard]] constexpr Charge makeCharge(double value, ChargeUnit unit) {
  return {value, unit};
}

[[nodiscard]] constexpr Conductance makeConductance(double value, ConductanceUnit unit) {
  return {value, unit};
}

[[nodiscard]] constexpr Energy makeEnergy(double value, EnergyUnit unit) {
  return {value, unit};
}

[[nodiscard]] constexpr Force makeForce(double value, ForceUnit unit) {
  return {value, unit};
}

[[nodiscard]] constexpr Pressure makePressure(double value, PressureUnit unit) {
  return {value, unit};
}

}  // namespace volt::math
