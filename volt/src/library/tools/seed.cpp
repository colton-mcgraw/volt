#include "seed.hpp"

#include <cstdint>
#include <set>
#include <math/electrical/e_series.hpp>
#include <math/electrical/engineering.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <string_view>

namespace volt::library::seed
{

    namespace
    {
        namespace el = math::electrical;

        constexpr std::string_view version = "2026.11.0";

        library make_library(std::string name, std::string title, std::string description)
        {
            library lib;
            lib.name = std::move(name);
            lib.title = std::move(title);
            lib.description = std::move(description);
            lib.version = std::string(version);
            lib.author = "Volt";
            lib.license = "CC0-1.0";
            return lib;
        }

        // Engineering notation with the digits a value needs: "4.7k", "10.2k".
        std::string eng(double value, int digits, std::string_view unit = {}, bool ascii = false)
        {
            el::engineering_format format;
            format.significant_digits = digits;
            format.unit = unit;
            format.ascii = ascii;
            return el::format_engineering(value, format);
        }

        // The preferred values of a series from `low` to `high`, both
        // included when they are members.
        std::vector<double> series_values(el::e_series series, double low, double high)
        {
            std::vector<double> values;
            const std::size_t count = el::e_series_size(series);
            for (int decade = -12; decade <= 7; ++decade)
            {
                for (std::size_t i = 0; i < count; ++i)
                {
                    const double raw = el::e_series_mantissa(series, i) * std::pow(10.0, decade - 2);
                    const double v = el::preferred_nearest(raw, series);
                    if (v >= low * (1 - 1e-9) && v <= high * (1 + 1e-9))
                        values.push_back(v);
                }
            }
            return values;
        }

        parameter param(std::string name, std::string value, std::string unit = {})
        {
            return parameter{std::move(name), std::move(value), std::move(unit), {}};
        }

        parameter quantity(std::string name, double value, std::string unit, int digits = 3)
        {
            return parameter::quantity(std::move(name), value, std::move(unit), digits);
        }

        // ---- resistors ---------------------------------------------------------

        struct chip_size
        {
            std::string_view imperial; // "0603"
            std::string_view metric;   // "1608"
            double power;              // W
            double voltage;            // V
        };

        constexpr chip_size resistor_sizes[] = {
            {"0402", "1005", 0.0625, 50.0},
            {"0603", "1608", 0.1, 75.0},
            {"0805", "2012", 0.125, 150.0},
            {"1206", "3216", 0.25, 200.0},
        };

        // Yageo's value code: the prefix letter, or R for ohms, in place of
        // the decimal point: 4K7, 49R9, 10K, 1M.
        std::string rkm_code(double ohms)
        {
            std::string text = eng(ohms, 3);
            char letter = 'R';
            if (!text.empty() && (text.back() == 'k' || text.back() == 'M'))
            {
                letter = text.back() == 'k' ? 'K' : 'M';
                text.pop_back();
            }
            const std::size_t point = text.find('.');
            if (point == std::string::npos)
                return text + letter;
            text[point] = letter;
            return text;
        }

        library resistors()
        {
            library lib = make_library("volt-resistors", "Volt Resistors",
                                       "Thick film chip resistors: E96 at 1% from 10Ω to 1MΩ, E24 at 5% from 1Ω to "
                                       "10MΩ, and zero-ohm jumpers, in 0402, 0603, 0805 and 1206.");
            struct range
            {
                el::e_series series;
                double low, high;
                int tolerance; // %
                char code;     // Yageo's tolerance letter
            };
            const range ranges[] = {
                {el::e_series::e96, 10.0, 1e6, 1, 'F'},
                {el::e_series::e24, 1.0, 10e6, 5, 'J'},
            };
            for (const range &r : ranges)
            {
                for (const chip_size &size : resistor_sizes)
                {
                    for (const double ohms : series_values(r.series, r.low, r.high))
                    {
                        part p;
                        const std::string value = eng(ohms, 3);
                        p.id = std::format("R_{}_{}_{}%", size.imperial, value, r.tolerance);
                        p.name = std::format("{}Ω {}% {}", value, r.tolerance, size.imperial);
                        p.category = "Passives/Resistors/Chip";
                        p.description = std::format("Thick film chip resistor, {}Ω ±{}%, {}W, {}V, {} ({} metric)",
                                                    value, r.tolerance, eng(size.power, 3), eng(size.voltage, 3),
                                                    size.imperial, size.metric);
                        p.manufacturer = "Yageo";
                        p.mpn = std::format("RC{}{}R-07{}L", size.imperial, r.code, rkm_code(ohms));
                        p.value = value;
                        p.package = std::string(size.imperial);
                        p.prefix = "R";
                        p.device = "resistor";
                        p.symbol = "R";
                        p.footprint = std::format("Resistor_SMD:R_{}_{}Metric", size.imperial, size.metric);
                        if (size.imperial == "1206")
                            p.model = "parts/R_1206_3216Metric.step";
                        p.keywords = {"resistor", "chip", "smd", "thick film"};
                        p.parameters = {
                            quantity("Resistance", ohms, "Ω"),
                            param("Tolerance", std::to_string(r.tolerance), "%"),
                            quantity("Power rating", size.power, "W"),
                            quantity("Voltage rating", size.voltage, "V"),
                            param("Temperature coefficient", r.tolerance == 1 ? "100" : "200", "ppm/K"),
                            param("Series", r.series == el::e_series::e96 ? "E96" : "E24"),
                        };
                        lib.parts.push_back(std::move(p));
                    }
                }
            }
            for (const chip_size &size : resistor_sizes)
            {
                part p;
                p.id = std::format("R_{}_0R", size.imperial);
                p.name = std::format("0Ω jumper {}", size.imperial);
                p.category = "Passives/Resistors/Jumpers";
                p.description = std::format("Zero-ohm chip jumper, {} ({} metric)", size.imperial, size.metric);
                p.manufacturer = "Yageo";
                p.mpn = std::format("RC{}JR-070RL", size.imperial);
                p.value = "0";
                p.package = std::string(size.imperial);
                p.prefix = "R";
                p.device = "resistor";
                p.symbol = "R";
                p.footprint = std::format("Resistor_SMD:R_{}_{}Metric", size.imperial, size.metric);
                if (size.imperial == "1206")
                    p.model = "parts/R_1206_3216Metric.step";
                p.keywords = {"resistor", "jumper", "zero ohm", "smd"};
                p.parameters = {quantity("Resistance", 0.0, "Ω")};
                lib.parts.push_back(std::move(p));
            }
            return lib;
        }

        // ---- capacitors --------------------------------------------------------

        // The EIA three-character code: two digits and a power of ten, or R
        // for the decimal point below ten: 104 is 10e4, 4R7 is 4.7.
        std::string eia_code(double units)
        {
            const double tenths = std::round(units * 10.0);
            if (tenths < 100.0)
            {
                const auto t = static_cast<long long>(tenths);
                return std::format("{}R{}", t / 10, t % 10);
            }
            auto digits = static_cast<long long>(std::round(units));
            int power = 0;
            while (digits >= 100)
            {
                digits /= 10;
                ++power;
            }
            return std::format("{:02}{}", digits, power);
        }

        // Yageo's voltage digit for the CC series.
        char yageo_voltage(double volts)
        {
            if (volts <= 6.3)
                return '5';
            if (volts <= 10.0)
                return '6';
            if (volts <= 16.0)
                return '7';
            if (volts <= 25.0)
                return '8';
            return '9';
        }

        struct dielectric
        {
            std::string_view name;
            std::string_view yageo; // in the part number
            int tolerance;          // %
            char tolerance_code;
            el::e_series series;
            // The range made in each size, 0402 0603 0805 1206.
            double low[4];
            double high[4];
        };

        // The voltage ratings a value is made in, by dielectric and value.
        std::vector<double> voltages_for(std::string_view dielectric_name, double farads)
        {
            if (dielectric_name == "C0G")
                return {50.0};
            if (dielectric_name == "X7R")
            {
                if (farads <= 10e-9 * 1.0001)
                    return {50.0};
                if (farads <= 100e-9 * 1.0001)
                    return {16.0, 50.0};
                if (farads <= 1e-6 * 1.0001)
                    return {16.0, 25.0};
                return {10.0, 16.0};
            }
            if (farads < 10e-6 * 0.9999)
                return {10.0, 16.0, 25.0};
            return {6.3, 10.0, 16.0};
        }

        void ceramic(library &lib)
        {
            const dielectric dielectrics[] = {
                {"C0G", "NPO", 5, 'J', el::e_series::e12, {1e-12, 1e-12, 1e-12, 1e-12}, {1e-9, 4.7e-9, 10e-9, 22e-9}},
                {"X7R", "X7R", 10, 'K', el::e_series::e12, {100e-12, 100e-12, 1e-9, 1e-9}, {100e-9, 1e-6, 4.7e-6, 10e-6}},
                {"X5R", "X5R", 20, 'M', el::e_series::e6, {1e-6, 1e-6, 1e-6, 4.7e-6}, {4.7e-6, 22e-6, 47e-6, 100e-6}},
            };
            for (const dielectric &d : dielectrics)
            {
                for (std::size_t s = 0; s < 4; ++s)
                {
                    const chip_size &size = resistor_sizes[s];
                    for (const double farads : series_values(d.series, d.low[s], d.high[s]))
                    {
                        // Below 10 pF a C0G part's tolerance is absolute.
                        const bool small = d.name == "C0G" && farads < 10e-12 * 0.9999;
                        for (const double volts : voltages_for(d.name, farads))
                        {
                            part p;
                            const std::string value = eng(farads, 2, "F");
                            const std::string volts_text = eng(volts, 2);
                            p.id = std::format("C_{}_{}_{}_{}V", size.imperial, eng(farads, 2, {}, true), d.name,
                                               volts_text);
                            p.name = std::format("{} {}V {} {}", value, volts_text, d.name, size.imperial);
                            p.category = "Passives/Capacitors/Ceramic";
                            p.description =
                                std::format("Multilayer ceramic chip capacitor, {} {}, {}V, {} ({} metric)", value,
                                            small ? std::string("±0.25pF") : std::format("±{}%", d.tolerance),
                                            volts_text, d.name, size.imperial, size.metric);
                            p.manufacturer = "Yageo";
                            p.mpn = std::format("CC{}{}R{}{}B{}{}", size.imperial, small ? 'C' : d.tolerance_code,
                                                d.yageo, yageo_voltage(volts), d.name == "C0G" ? 'N' : 'B',
                                                eia_code(farads * 1e12));
                            p.value = value;
                            p.package = std::string(size.imperial);
                            p.prefix = "C";
                            p.device = "capacitor";
                            p.symbol = "C";
                            p.footprint = std::format("Capacitor_SMD:C_{}_{}Metric", size.imperial, size.metric);
                            p.keywords = {"capacitor", "ceramic", "mlcc", "smd"};
                            p.parameters = {
                                quantity("Capacitance", farads, "F"),
                                small ? param("Tolerance", "0.25", "pF")
                                      : param("Tolerance", std::to_string(d.tolerance), "%"),
                                quantity("Voltage rating", volts, "V"),
                                param("Dielectric", std::string(d.name)),
                            };
                            lib.parts.push_back(std::move(p));
                        }
                    }
                }
            }
        }

        // Radial aluminium electrolytics: the can grows with the charge it
        // holds. Diameter and lead pitch by capacitance times voltage.
        struct can
        {
            double max_cv; // µF·V
            std::string_view diameter;
            std::string_view pitch;
        };

        constexpr can cans[] = {
            {100.0, "5.0", "2.00"},  {470.0, "6.3", "2.50"},  {1500.0, "8.0", "3.50"},
            {3300.0, "10.0", "5.00"}, {10000.0, "12.5", "5.00"}, {22000.0, "16.0", "7.50"},
        };

        void electrolytic(library &lib)
        {
            struct rating
            {
                double volts;
                std::string_view code; // Panasonic's
            };
            constexpr rating ratings[] = {{6.3, "0J"}, {10.0, "1A"}, {16.0, "1C"}, {25.0, "1E"}, {35.0, "1V"}, {50.0, "1H"}};
            for (const double farads : series_values(el::e_series::e6, 1e-6, 2.2e-3))
            {
                for (const rating &r : ratings)
                {
                    const double cv = farads * 1e6 * r.volts;
                    const can *size = nullptr;
                    for (const can &c : cans)
                    {
                        if (cv <= c.max_cv)
                        {
                            size = &c;
                            break;
                        }
                    }
                    if (size == nullptr)
                        continue;
                    part p;
                    const std::string value = eng(farads, 2, "F");
                    const std::string volts = eng(r.volts, 2);
                    p.id = std::format("CP_Radial_{}_{}V", eng(farads, 2, {}, true), volts);
                    p.name = std::format("{} {}V electrolytic D{}mm", value, volts, size->diameter);
                    p.category = "Passives/Capacitors/Electrolytic";
                    p.description = std::format("Aluminium electrolytic capacitor, radial, {} ±20%, {}V, 105°C, "
                                                "D{}mm, pitch {}mm",
                                                value, volts, size->diameter, size->pitch);
                    p.manufacturer = "Panasonic";
                    p.mpn = std::format("EEU-FR{}{}", r.code, eia_code(farads * 1e6));
                    p.value = value;
                    p.package = std::format("Radial D{}mm", size->diameter);
                    p.prefix = "C";
                    p.device = "capacitor";
                    p.symbol = "C_Polarized";
                    p.footprint = std::format("Capacitor_THT:CP_Radial_D{}mm_P{}mm", size->diameter, size->pitch);
                    if (size->diameter == "6.3")
                        p.model = "parts/CP_Radial_D6.3mm_P2.50mm.step";
                    p.keywords = {"capacitor", "electrolytic", "polarized", "aluminium", "tht", "radial"};
                    p.parameters = {
                        quantity("Capacitance", farads, "F"),
                        param("Tolerance", "20", "%"),
                        quantity("Voltage rating", r.volts, "V"),
                        param("Temperature rating", "105", "°C"),
                        param("Diameter", std::string(size->diameter), "mm"),
                    };
                    lib.parts.push_back(std::move(p));
                }
            }
        }

        library capacitors()
        {
            library lib = make_library("volt-capacitors", "Volt Capacitors",
                                       "Ceramic chip capacitors (C0G, X7R, X5R) in 0402 to 1206, and radial "
                                       "aluminium electrolytics from 1µF to 2.2mF.");
            ceramic(lib);
            electrolytic(lib);
            return lib;
        }

        // ---- inductors ---------------------------------------------------------

        library inductors()
        {
            library lib = make_library("volt-inductors", "Volt Inductors",
                                       "Multilayer chip inductors in 0402 to 1206, and shielded SMD power inductors.");
            const double low[4] = {1e-9, 1e-9, 10e-9, 100e-9};
            const double high[4] = {100e-9, 1e-6, 10e-6, 47e-6};
            for (std::size_t s = 0; s < 4; ++s)
            {
                const chip_size &size = resistor_sizes[s];
                for (const double henries : series_values(el::e_series::e12, low[s], high[s]))
                {
                    part p;
                    const std::string value = eng(henries, 2, "H");
                    p.id = std::format("L_{}_{}", size.imperial, eng(henries, 2, "H", true));
                    p.name = std::format("{} {} inductor", value, size.imperial);
                    p.category = "Passives/Inductors/Chip";
                    p.description = std::format("Multilayer chip inductor, {} ±{}%, {} ({} metric)", value,
                                                henries < 100e-9 ? 5 : 10, size.imperial, size.metric);
                    p.value = value;
                    p.package = std::string(size.imperial);
                    p.prefix = "L";
                    p.device = "inductor";
                    p.symbol = "L";
                    p.footprint = std::format("Inductor_SMD:L_{}_{}Metric", size.imperial, size.metric);
                    p.keywords = {"inductor", "chip", "multilayer", "smd"};
                    p.parameters = {
                        quantity("Inductance", henries, "H"),
                        param("Tolerance", henries < 100e-9 ? "5" : "10", "%"),
                    };
                    lib.parts.push_back(std::move(p));
                }
            }
            for (const double henries : series_values(el::e_series::e6, 1e-6, 1e-3))
            {
                part p;
                const std::string value = eng(henries, 2, "H");
                p.id = std::format("L_SRR1260_{}", eng(henries, 2, "H", true));
                p.name = std::format("{} power inductor 12x12mm", value);
                p.category = "Passives/Inductors/Power";
                p.description = std::format("Shielded SMD power inductor, {} ±20%, 12.5x12.5x6mm", value);
                p.manufacturer = "Bourns";
                p.mpn = std::format("SRR1260-{}M", eia_code(henries * 1e6));
                p.value = value;
                p.package = "SRR1260";
                p.prefix = "L";
                p.device = "inductor";
                p.symbol = "L";
                p.footprint = "Inductor_SMD:L_Bourns_SRR1260";
                p.keywords = {"inductor", "power", "shielded", "smd", "buck"};
                p.parameters = {
                    quantity("Inductance", henries, "H"),
                    param("Tolerance", "20", "%"),
                };
                lib.parts.push_back(std::move(p));
            }
            return lib;
        }

        // ---- discretes ---------------------------------------------------------

        struct package_info
        {
            std::string_view name;
            std::string_view footprint;
            std::string_view model;
        };

        void leds(library &lib)
        {
            struct color
            {
                std::string_view name;
                std::string_view wavelength; // nm, empty for white
                double forward;              // V
            };
            constexpr color colors[] = {
                {"Red", "630", 2.0},  {"Orange", "605", 2.0}, {"Yellow", "590", 2.1},
                {"Green", "570", 2.2}, {"Blue", "470", 3.0},  {"White", "", 3.0},
            };
            constexpr package_info packages[] = {
                {"0603", "LED_SMD:LED_0603_1608Metric", ""},
                {"0805", "LED_SMD:LED_0805_2012Metric", ""},
                {"1206", "LED_SMD:LED_1206_3216Metric", ""},
                {"3mm", "LED_THT:LED_D3.0mm", ""},
                {"5mm", "LED_THT:LED_D5.0mm", "parts/LED_D5.0mm.step"},
            };
            for (const package_info &pkg : packages)
            {
                const bool tht = pkg.name.ends_with("mm");
                for (const color &c : colors)
                {
                    part p;
                    p.id = std::format("LED_{}_{}", pkg.name, c.name);
                    p.name = std::format("LED {} {}", c.name, pkg.name);
                    p.category = "Optoelectronics/LEDs";
                    p.description =
                        std::format("{} LED, {}{}, {}V at 20mA", c.name, tht ? "through hole, " : "chip, ",
                                    tht ? std::format("{} diameter", pkg.name) : std::string(pkg.name), eng(c.forward, 2));
                    p.value = std::string(c.name);
                    p.package = std::string(pkg.name);
                    p.prefix = "D";
                    p.device = "led";
                    p.symbol = "LED";
                    p.footprint = std::string(pkg.footprint);
                    p.model = std::string(pkg.model);
                    p.keywords = {"led", "light", "indicator", tht ? "tht" : "smd"};
                    p.parameters.push_back(param("Color", std::string(c.name)));
                    if (!c.wavelength.empty())
                        p.parameters.push_back(param("Wavelength", std::string(c.wavelength), "nm"));
                    p.parameters.push_back(quantity("Forward voltage", c.forward, "V"));
                    p.parameters.push_back(quantity("Forward current", 0.02, "A"));
                    lib.parts.push_back(std::move(p));
                }
            }
        }

        struct diode
        {
            std::string_view mpn;
            std::string_view kind; // Rectifier, Schottky, Small signal
            std::string_view package;
            double reverse; // V
            double current; // A
            std::string_view manufacturer;
        };

        package_info diode_package(std::string_view package)
        {
            if (package == "DO-35")
                return {package, "Diode_THT:D_DO-35_SOD27_P7.62mm_Horizontal", ""};
            if (package == "DO-41")
                return {package, "Diode_THT:D_DO-41_SOD81_P10.16mm_Horizontal", ""};
            if (package == "DO-201AD")
                return {package, "Diode_THT:D_DO-201AD_P15.24mm_Horizontal", ""};
            if (package == "SOD-123")
                return {package, "Diode_SMD:D_SOD-123", ""};
            if (package == "SOD-323")
                return {package, "Diode_SMD:D_SOD-323", ""};
            if (package == "SMA")
                return {package, "Diode_SMD:D_SMA", ""};
            return {package, "Package_TO_SOT_SMD:SOT-23", ""};
        }

        void diodes(library &lib)
        {
            constexpr diode list[] = {
                {"1N4148", "Small signal", "DO-35", 100.0, 0.2, "onsemi"},
                {"1N4148W", "Small signal", "SOD-123", 100.0, 0.15, "Diodes Incorporated"},
                {"1N4148WS", "Small signal", "SOD-323", 75.0, 0.15, "Diodes Incorporated"},
                {"BAV99", "Small signal", "SOT-23", 70.0, 0.2, "Nexperia"},
                {"1N4001", "Rectifier", "DO-41", 50.0, 1.0, "onsemi"},
                {"1N4002", "Rectifier", "DO-41", 100.0, 1.0, "onsemi"},
                {"1N4003", "Rectifier", "DO-41", 200.0, 1.0, "onsemi"},
                {"1N4004", "Rectifier", "DO-41", 400.0, 1.0, "onsemi"},
                {"1N4005", "Rectifier", "DO-41", 600.0, 1.0, "onsemi"},
                {"1N4006", "Rectifier", "DO-41", 800.0, 1.0, "onsemi"},
                {"1N4007", "Rectifier", "DO-41", 1000.0, 1.0, "onsemi"},
                {"1N5400", "Rectifier", "DO-201AD", 50.0, 3.0, "onsemi"},
                {"1N5401", "Rectifier", "DO-201AD", 100.0, 3.0, "onsemi"},
                {"1N5402", "Rectifier", "DO-201AD", 200.0, 3.0, "onsemi"},
                {"1N5404", "Rectifier", "DO-201AD", 400.0, 3.0, "onsemi"},
                {"1N5406", "Rectifier", "DO-201AD", 600.0, 3.0, "onsemi"},
                {"1N5408", "Rectifier", "DO-201AD", 1000.0, 3.0, "onsemi"},
                {"M7", "Rectifier", "SMA", 1000.0, 1.0, "Diodes Incorporated"},
                {"1N5817", "Schottky", "DO-41", 20.0, 1.0, "onsemi"},
                {"1N5818", "Schottky", "DO-41", 30.0, 1.0, "onsemi"},
                {"1N5819", "Schottky", "DO-41", 40.0, 1.0, "onsemi"},
                {"1N5820", "Schottky", "DO-201AD", 20.0, 3.0, "onsemi"},
                {"1N5821", "Schottky", "DO-201AD", 30.0, 3.0, "onsemi"},
                {"1N5822", "Schottky", "DO-201AD", 40.0, 3.0, "onsemi"},
                {"B5819W", "Schottky", "SOD-123", 40.0, 1.0, "Diodes Incorporated"},
                {"BAT54", "Schottky", "SOT-23", 30.0, 0.2, "Nexperia"},
                {"BAT54S", "Schottky", "SOT-23", 30.0, 0.2, "Nexperia"},
                {"BAT54C", "Schottky", "SOT-23", 30.0, 0.2, "Nexperia"},
                {"SS14", "Schottky", "SMA", 40.0, 1.0, "onsemi"},
                {"SS16", "Schottky", "SMA", 60.0, 1.0, "onsemi"},
            };
            for (const diode &d : list)
            {
                const package_info pkg = diode_package(d.package);
                part p;
                p.id = std::string(d.mpn);
                p.name = std::string(d.mpn);
                p.category = std::format("Discretes/Diodes/{}", d.kind);
                p.description = std::format("{} diode, {}V {}A, {}", d.kind, eng(d.reverse, 3), eng(d.current, 3),
                                            d.package);
                p.manufacturer = std::string(d.manufacturer);
                p.mpn = std::string(d.mpn);
                p.value = std::string(d.mpn);
                p.package = std::string(d.package);
                p.prefix = "D";
                p.device = "diode";
                p.symbol = d.kind == "Schottky" ? "D_Schottky" : "D";
                if (d.mpn == "BAV99" || d.mpn == "BAT54S")
                    p.symbol = "D_Dual_Series";
                else if (d.mpn == "BAT54C")
                    p.symbol = "D_Dual_CommonCathode";
                p.footprint = std::string(pkg.footprint);
                p.keywords = {"diode", d.kind == "Schottky" ? "schottky" : d.kind == "Rectifier" ? "rectifier" : "signal"};
                p.parameters = {
                    quantity("Reverse voltage", d.reverse, "V"),
                    quantity("Forward current", d.current, "A"),
                };
                lib.parts.push_back(std::move(p));
            }

            // Zener diodes, every E24 voltage the BZX84 and BZX55 series come in.
            constexpr std::string_view zener_voltages[] = {
                "2V4", "2V7", "3V0", "3V3", "3V6", "3V9", "4V3", "4V7", "5V1", "5V6", "6V2", "6V8", "7V5", "8V2", "9V1",
                "10",  "11",  "12",  "13",  "15",  "16",  "18",  "20",  "22",  "24",  "27",  "30",  "33",  "36",  "39",
            };
            struct zener_series
            {
                std::string_view prefix;
                std::string_view package;
                std::string_view footprint;
                double power;
            };
            constexpr zener_series zeners[] = {
                {"BZX84C", "SOT-23", "Package_TO_SOT_SMD:SOT-23", 0.25},
                {"BZX55C", "DO-35", "Diode_THT:D_DO-35_SOD27_P7.62mm_Horizontal", 0.5},
            };
            for (const zener_series &z : zeners)
            {
                for (const std::string_view code : zener_voltages)
                {
                    // "5V1" is 5.1 V: the V stands in for the decimal point.
                    std::string decimal(code);
                    std::replace(decimal.begin(), decimal.end(), 'V', '.');
                    const double volts = el::parse_engineering(decimal)->value;
                    const std::string volts_text = eng(volts, 2);
                    part p;
                    p.id = std::format("{}{}", z.prefix, code);
                    p.name = p.id;
                    p.category = "Discretes/Diodes/Zener";
                    p.description = std::format("Zener diode, {}V ±5%, {}W, {}", volts_text, eng(z.power, 2), z.package);
                    p.manufacturer = z.prefix == "BZX84C" ? "Nexperia" : "Vishay";
                    p.mpn = p.id;
                    p.value = std::format("{}V", volts_text);
                    p.package = std::string(z.package);
                    p.prefix = "D";
                    p.device = "zener";
                    p.symbol = "D_Zener";
                    p.footprint = std::string(z.footprint);
                    p.keywords = {"diode", "zener", "reference", "clamp"};
                    p.parameters = {
                        quantity("Zener voltage", volts, "V"),
                        param("Tolerance", "5", "%"),
                        quantity("Power rating", z.power, "W"),
                    };
                    lib.parts.push_back(std::move(p));
                }
            }
        }

        void transistors(library &lib)
        {
            struct transistor
            {
                std::string_view mpn;
                std::string_view type; // NPN, PNP, N-channel, P-channel
                std::string_view package;
                double voltage; // V, Vce or Vds
                double current; // A
                std::string_view manufacturer;
                std::string_view note;
            };
            constexpr transistor list[] = {
                {"BC547B", "NPN", "TO-92", 45.0, 0.1, "onsemi", "general purpose"},
                {"BC557B", "PNP", "TO-92", 45.0, 0.1, "onsemi", "general purpose"},
                {"BC847B", "NPN", "SOT-23", 45.0, 0.1, "Nexperia", "general purpose"},
                {"BC857B", "PNP", "SOT-23", 45.0, 0.1, "Nexperia", "general purpose"},
                {"2N3904", "NPN", "TO-92", 40.0, 0.2, "onsemi", "general purpose"},
                {"2N3906", "PNP", "TO-92", 40.0, 0.2, "onsemi", "general purpose"},
                {"MMBT3904", "NPN", "SOT-23", 40.0, 0.2, "onsemi", "general purpose"},
                {"MMBT3906", "PNP", "SOT-23", 40.0, 0.2, "onsemi", "general purpose"},
                {"PN2222A", "NPN", "TO-92", 40.0, 1.0, "onsemi", "general purpose"},
                {"MMBT2222A", "NPN", "SOT-23", 40.0, 0.6, "onsemi", "general purpose"},
                {"BD139", "NPN", "TO-126", 80.0, 1.5, "STMicroelectronics", "medium power"},
                {"BD140", "PNP", "TO-126", 80.0, 1.5, "STMicroelectronics", "medium power"},
                {"TIP31C", "NPN", "TO-220", 100.0, 3.0, "onsemi", "power"},
                {"TIP32C", "PNP", "TO-220", 100.0, 3.0, "onsemi", "power"},
                {"TIP120", "NPN", "TO-220", 60.0, 5.0, "onsemi", "Darlington"},
                {"2N7002", "N-channel", "SOT-23", 60.0, 0.115, "onsemi", "small signal"},
                {"BSS138", "N-channel", "SOT-23", 50.0, 0.22, "onsemi", "logic level"},
                {"BSS84", "P-channel", "SOT-23", 50.0, 0.13, "onsemi", "small signal"},
                {"AO3400A", "N-channel", "SOT-23", 30.0, 5.7, "Alpha & Omega", "logic level"},
                {"AO3401A", "P-channel", "SOT-23", 30.0, 4.0, "Alpha & Omega", "logic level"},
                {"IRLML2502", "N-channel", "SOT-23", 20.0, 4.2, "Infineon", "logic level"},
                {"IRLZ44N", "N-channel", "TO-220", 55.0, 47.0, "Infineon", "logic level"},
                {"IRF540N", "N-channel", "TO-220", 100.0, 33.0, "Infineon", "power"},
                {"IRF9540N", "P-channel", "TO-220", 100.0, 23.0, "Infineon", "power"},
            };
            for (const transistor &t : list)
            {
                const bool mosfet = t.type.ends_with("channel");
                part p;
                p.id = std::string(t.mpn);
                p.name = std::string(t.mpn);
                p.category = mosfet ? "Discretes/Transistors/MOSFET" : "Discretes/Transistors/BJT";
                p.description = std::format("{} {} {}, {}V {}A, {}", t.type, t.note, mosfet ? "MOSFET" : "transistor",
                                            eng(t.voltage, 3), eng(t.current, 3), t.package);
                p.manufacturer = std::string(t.manufacturer);
                p.mpn = std::string(t.mpn);
                p.value = std::string(t.mpn);
                p.package = std::string(t.package);
                p.prefix = "Q";
                p.device = mosfet ? "mosfet" : "bjt";
                if (mosfet)
                    p.symbol = t.type == "N-channel" ? "Q_NMOS" : "Q_PMOS";
                else
                    p.symbol = t.type == "NPN" ? "Q_NPN" : "Q_PNP";
                if (t.package == "TO-92")
                    p.footprint = "Package_TO_SOT_THT:TO-92_Inline";
                else if (t.package == "TO-126")
                    p.footprint = "Package_TO_SOT_THT:TO-126-3_Vertical";
                else if (t.package == "TO-220")
                    p.footprint = "Package_TO_SOT_THT:TO-220-3_Vertical";
                else
                    p.footprint = "Package_TO_SOT_SMD:SOT-23";
                p.keywords = {"transistor", mosfet ? "mosfet" : "bjt", std::string(t.type == "NPN" ? "npn"
                                                                                  : t.type == "PNP" ? "pnp"
                                                                                  : t.type == "N-channel" ? "nmos"
                                                                                                          : "pmos")};
                p.parameters = {
                    param("Type", std::string(t.type)),
                    quantity(mosfet ? "Drain-source voltage" : "Collector-emitter voltage", t.voltage, "V"),
                    quantity(mosfet ? "Drain current" : "Collector current", t.current, "A"),
                };
                lib.parts.push_back(std::move(p));
            }
        }

        library discretes()
        {
            library lib = make_library("volt-discretes", "Volt Discretes",
                                       "LEDs, diodes (signal, rectifier, Schottky, Zener) and transistors (BJT, "
                                       "MOSFET).");
            leds(lib);
            diodes(lib);
            transistors(lib);
            return lib;
        }

        // ---- integrated circuits -----------------------------------------------

        package_info ic_package(std::string_view package)
        {
            constexpr package_info packages[] = {
                {"SOIC-8", "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm", "parts/SOIC-8_3.9x4.9mm_P1.27mm.step"},
                {"SOIC-8W", "Package_SO:SOIC-8_5.3x5.3mm_P1.27mm", ""},
                {"SOIC-14", "Package_SO:SOIC-14_3.9x8.7mm_P1.27mm", ""},
                {"SOIC-16", "Package_SO:SOIC-16_3.9x9.9mm_P1.27mm", ""},
                {"SSOP-28", "Package_SO:SSOP-28_5.3x10.2mm_P0.65mm", ""},
                {"TSSOP-20", "Package_SO:TSSOP-20_4.4x6.5mm_P0.65mm", ""},
                {"DIP-8", "Package_DIP:DIP-8_W7.62mm", ""},
                {"DIP-28", "Package_DIP:DIP-28_W7.62mm", ""},
                {"SOT-23", "Package_TO_SOT_SMD:SOT-23", ""},
                {"SOT-23-5", "Package_TO_SOT_SMD:SOT-23-5", ""},
                {"SOT-23-6", "Package_TO_SOT_SMD:SOT-23-6", ""},
                {"SOT-223", "Package_TO_SOT_SMD:SOT-223-3_TabPin2", ""},
                {"TO-220", "Package_TO_SOT_THT:TO-220-3_Vertical", ""},
                {"TO-263-5", "Package_TO_SOT_SMD:TO-263-5_TabPin3", ""},
                {"TQFP-32", "Package_QFP:TQFP-32_7x7mm_P0.8mm", ""},
                {"TQFP-48", "Package_QFP:TQFP-48_7x7mm_P0.5mm", ""},
                {"LQFP-48", "Package_QFP:LQFP-48_7x7mm_P0.5mm", ""},
                {"QFN-48", "Package_DFN_QFN:QFN-48-1EP_7x7mm_P0.5mm_EP5.6x5.6mm", ""},
                {"QFN-56", "Package_DFN_QFN:QFN-56-1EP_7x7mm_P0.4mm_EP3.2x3.2mm", ""},
            };
            for (const package_info &p : packages)
                if (p.name == package)
                    return p;
            return {package, "", ""};
        }

        library ics()
        {
            library lib = make_library("volt-ics", "Volt ICs",
                                       "A starter set of integrated circuits: op-amps, regulators, microcontrollers, "
                                       "memory, timers and interface chips.");
            struct ic
            {
                std::string_view mpn;
                std::string_view value; // the generic name
                std::string_view category;
                std::string_view package;
                std::string_view manufacturer;
                std::string_view symbol;
                std::string_view description;
                std::string_view keywords;  // comma separated
                std::string_view datasheet; // TI's are at a stable address
            };
            constexpr ic list[] = {
                // Op-amps
                {"LM358DR", "LM358", "ICs/Amplifiers/Op-Amps", "SOIC-8", "Texas Instruments", "OpAmp_Dual",
                 "Dual general purpose op-amp, 3V to 32V supply", "opamp,amplifier,dual", "lm358"},
                {"LM358P", "LM358", "ICs/Amplifiers/Op-Amps", "DIP-8", "Texas Instruments", "OpAmp_Dual",
                 "Dual general purpose op-amp, 3V to 32V supply", "opamp,amplifier,dual", "lm358"},
                {"LM324DR", "LM324", "ICs/Amplifiers/Op-Amps", "SOIC-14", "Texas Instruments", "OpAmp_Quad",
                 "Quad general purpose op-amp, 3V to 32V supply", "opamp,amplifier,quad", "lm324"},
                {"TL072CDR", "TL072", "ICs/Amplifiers/Op-Amps", "SOIC-8", "Texas Instruments", "OpAmp_Dual",
                 "Dual low noise JFET-input op-amp", "opamp,amplifier,dual,jfet,audio", "tl072"},
                {"TL074CDR", "TL074", "ICs/Amplifiers/Op-Amps", "SOIC-14", "Texas Instruments", "OpAmp_Quad",
                 "Quad low noise JFET-input op-amp", "opamp,amplifier,quad,jfet,audio", "tl074"},
                {"NE5532DR", "NE5532", "ICs/Amplifiers/Op-Amps", "SOIC-8", "Texas Instruments", "OpAmp_Dual",
                 "Dual low noise op-amp for audio", "opamp,amplifier,dual,audio,low noise", "ne5532"},
                {"OPA2134UA", "OPA2134", "ICs/Amplifiers/Op-Amps", "SOIC-8", "Texas Instruments", "OpAmp_Dual",
                 "Dual high performance audio op-amp", "opamp,amplifier,dual,audio", "opa2134"},
                {"OPA2340UA", "OPA2340", "ICs/Amplifiers/Op-Amps", "SOIC-8", "Texas Instruments", "OpAmp_Dual",
                 "Dual rail-to-rail CMOS op-amp, 2.7V to 5.5V", "opamp,amplifier,dual,rail-to-rail", "opa2340"},
                {"LMV358IDR", "LMV358", "ICs/Amplifiers/Op-Amps", "SOIC-8", "Texas Instruments", "OpAmp_Dual",
                 "Dual low voltage rail-to-rail output op-amp", "opamp,amplifier,dual,rail-to-rail", "lmv358"},
                {"MCP6002-I/SN", "MCP6002", "ICs/Amplifiers/Op-Amps", "SOIC-8", "Microchip", "OpAmp_Dual",
                 "Dual 1MHz rail-to-rail op-amp, 1.8V to 6V", "opamp,amplifier,dual,rail-to-rail", ""},
                {"MCP6001T-I/OT", "MCP6001", "ICs/Amplifiers/Op-Amps", "SOT-23-5", "Microchip", "OpAmp",
                 "Single 1MHz rail-to-rail op-amp, 1.8V to 6V", "opamp,amplifier,single,rail-to-rail", ""},
                // Linear regulators
                {"L7805CV", "7805", "ICs/Power/Linear Regulators", "TO-220", "STMicroelectronics", "Regulator_Linear",
                 "5V 1.5A positive linear regulator", "regulator,ldo,linear,5v", ""},
                {"L7809CV", "7809", "ICs/Power/Linear Regulators", "TO-220", "STMicroelectronics", "Regulator_Linear",
                 "9V 1.5A positive linear regulator", "regulator,linear,9v", ""},
                {"L7812CV", "7812", "ICs/Power/Linear Regulators", "TO-220", "STMicroelectronics", "Regulator_Linear",
                 "12V 1.5A positive linear regulator", "regulator,linear,12v", ""},
                {"L7815CV", "7815", "ICs/Power/Linear Regulators", "TO-220", "STMicroelectronics", "Regulator_Linear",
                 "15V 1.5A positive linear regulator", "regulator,linear,15v", ""},
                {"LM317T", "LM317", "ICs/Power/Linear Regulators", "TO-220", "Texas Instruments", "Regulator_Adjustable",
                 "Adjustable 1.2V to 37V 1.5A positive linear regulator", "regulator,linear,adjustable", "lm317"},
                {"AMS1117-3.3", "AMS1117-3.3", "ICs/Power/Linear Regulators", "SOT-223", "Advanced Monolithic Systems",
                 "Regulator_Linear", "3.3V 1A low dropout regulator", "regulator,ldo,linear,3.3v", ""},
                {"AMS1117-5.0", "AMS1117-5.0", "ICs/Power/Linear Regulators", "SOT-223", "Advanced Monolithic Systems",
                 "Regulator_Linear", "5V 1A low dropout regulator", "regulator,ldo,linear,5v", ""},
                {"AMS1117-1.8", "AMS1117-1.8", "ICs/Power/Linear Regulators", "SOT-223", "Advanced Monolithic Systems",
                 "Regulator_Linear", "1.8V 1A low dropout regulator", "regulator,ldo,linear,1.8v", ""},
                {"MCP1700T-3302E/TT", "MCP1700-3.3", "ICs/Power/Linear Regulators", "SOT-23", "Microchip",
                 "Regulator_Linear", "3.3V 250mA low quiescent current LDO", "regulator,ldo,linear,3.3v,low power", ""},
                {"MCP1700T-1802E/TT", "MCP1700-1.8", "ICs/Power/Linear Regulators", "SOT-23", "Microchip",
                 "Regulator_Linear", "1.8V 250mA low quiescent current LDO", "regulator,ldo,linear,1.8v,low power", ""},
                {"LP5907MFX-3.3/NOPB", "LP5907-3.3", "ICs/Power/Linear Regulators", "SOT-23-5", "Texas Instruments",
                 "Regulator_Linear_EN", "3.3V 250mA ultra low noise LDO", "regulator,ldo,linear,3.3v,low noise", "lp5907"},
                {"AP2112K-3.3TRG1", "AP2112-3.3", "ICs/Power/Linear Regulators", "SOT-23-5", "Diodes Incorporated",
                 "Regulator_Linear_EN", "3.3V 600mA CMOS LDO with enable", "regulator,ldo,linear,3.3v", ""},
                // Switching regulators
                {"LM2596S-5.0", "LM2596-5.0", "ICs/Power/Switching Regulators", "TO-263-5", "Texas Instruments",
                 "Regulator_Buck", "5V 3A step-down switching regulator, 150kHz", "regulator,buck,switching,5v", "lm2596"},
                {"TPS563200DDCR", "TPS563200", "ICs/Power/Switching Regulators", "SOT-23-6", "Texas Instruments",
                 "Regulator_Buck", "4.5V to 17V input, 3A synchronous step-down converter", "regulator,buck,switching",
                 "tps563200"},
                {"MP1584EN-LF-Z", "MP1584", "ICs/Power/Switching Regulators", "SOIC-8", "Monolithic Power Systems",
                 "Regulator_Buck", "4.5V to 28V input, 3A step-down converter", "regulator,buck,switching", ""},
                // Microcontrollers
                {"ATMEGA328P-AU", "ATmega328P", "ICs/Microcontrollers/AVR", "TQFP-32", "Microchip", "MCU",
                 "8-bit AVR microcontroller, 32KB flash, 2KB SRAM, 20MHz", "mcu,avr,arduino,8-bit", ""},
                {"ATMEGA328P-PU", "ATmega328P", "ICs/Microcontrollers/AVR", "DIP-28", "Microchip", "MCU",
                 "8-bit AVR microcontroller, 32KB flash, 2KB SRAM, 20MHz", "mcu,avr,arduino,8-bit", ""},
                {"ATTINY85-20SU", "ATtiny85", "ICs/Microcontrollers/AVR", "SOIC-8W", "Microchip", "MCU",
                 "8-bit AVR microcontroller, 8KB flash, 512B SRAM, 20MHz", "mcu,avr,8-bit", ""},
                {"STM32F103C8T6", "STM32F103C8", "ICs/Microcontrollers/ARM", "LQFP-48", "STMicroelectronics", "MCU",
                 "Arm Cortex-M3 microcontroller, 64KB flash, 20KB SRAM, 72MHz", "mcu,arm,cortex-m3,stm32,32-bit", ""},
                {"STM32F411CEU6", "STM32F411CE", "ICs/Microcontrollers/ARM", "QFN-48", "STMicroelectronics", "MCU",
                 "Arm Cortex-M4F microcontroller, 512KB flash, 128KB SRAM, 100MHz", "mcu,arm,cortex-m4,stm32,32-bit", ""},
                {"STM32G030F6P6", "STM32G030F6", "ICs/Microcontrollers/ARM", "TSSOP-20", "STMicroelectronics", "MCU",
                 "Arm Cortex-M0+ microcontroller, 32KB flash, 8KB SRAM, 64MHz", "mcu,arm,cortex-m0+,stm32,32-bit", ""},
                {"ATSAMD21G18A-AU", "ATSAMD21G18", "ICs/Microcontrollers/ARM", "TQFP-48", "Microchip", "MCU",
                 "Arm Cortex-M0+ microcontroller, 256KB flash, 32KB SRAM, USB", "mcu,arm,cortex-m0+,usb,32-bit", ""},
                {"RP2040", "RP2040", "ICs/Microcontrollers/ARM", "QFN-56", "Raspberry Pi", "MCU",
                 "Dual Arm Cortex-M0+ microcontroller, 264KB SRAM, external flash, USB", "mcu,arm,cortex-m0+,usb,pico",
                 ""},
                {"CH32V003F4P6", "CH32V003", "ICs/Microcontrollers/RISC-V", "TSSOP-20", "WCH", "MCU",
                 "RISC-V microcontroller, 16KB flash, 2KB SRAM, 48MHz", "mcu,risc-v,32-bit", ""},
                // Memory
                {"24LC256-I/SN", "24LC256", "ICs/Memory/EEPROM", "SOIC-8", "Microchip", "EEPROM_I2C",
                 "256Kbit I2C serial EEPROM", "eeprom,memory,i2c", ""},
                {"AT24C02D-SSHM-T", "AT24C02", "ICs/Memory/EEPROM", "SOIC-8", "Microchip", "EEPROM_I2C",
                 "2Kbit I2C serial EEPROM", "eeprom,memory,i2c", ""},
                {"23LC1024-I/SN", "23LC1024", "ICs/Memory/SRAM", "SOIC-8", "Microchip", "SRAM_SPI",
                 "1Mbit SPI serial SRAM", "sram,memory,spi", ""},
                {"W25Q128JVSIQ", "W25Q128", "ICs/Memory/Flash", "SOIC-8W", "Winbond", "Flash_SPI",
                 "128Mbit SPI NOR flash, quad I/O", "flash,memory,spi,qspi", ""},
                // Timers
                {"NE555DR", "NE555", "ICs/Timers", "SOIC-8", "Texas Instruments", "Timer_555",
                 "Precision timer, bipolar, 4.5V to 16V", "timer,555,oscillator", "ne555"},
                {"NE555P", "NE555", "ICs/Timers", "DIP-8", "Texas Instruments", "Timer_555",
                 "Precision timer, bipolar, 4.5V to 16V", "timer,555,oscillator", "ne555"},
                {"TLC555CDR", "TLC555", "ICs/Timers", "SOIC-8", "Texas Instruments", "Timer_555",
                 "LinCMOS timer, 2V to 15V", "timer,555,oscillator,cmos", "tlc555"},
                // Interface and logic
                {"SN65HVD230DR", "SN65HVD230", "ICs/Interface/CAN", "SOIC-8", "Texas Instruments", "CAN_Transceiver",
                 "3.3V CAN transceiver", "can,transceiver,bus", "sn65hvd230"},
                {"CH340G", "CH340G", "ICs/Interface/USB", "SOIC-16", "WCH", "USB_UART",
                 "USB to serial UART bridge", "usb,uart,serial,bridge", ""},
                {"FT232RL", "FT232RL", "ICs/Interface/USB", "SSOP-28", "FTDI", "USB_UART",
                 "USB to serial UART bridge", "usb,uart,serial,bridge", ""},
                {"74HC595D", "74HC595", "ICs/Logic", "SOIC-16", "Nexperia", "74HC595",
                 "8-bit serial-in parallel-out shift register with output latches", "logic,shift register,74hc", ""},
                {"74HC14D", "74HC14", "ICs/Logic", "SOIC-14", "Nexperia", "74HC14",
                 "Hex inverting Schmitt trigger", "logic,inverter,schmitt,74hc", ""},
                {"ULN2003ADR", "ULN2003A", "ICs/Drivers", "SOIC-16", "Texas Instruments", "ULN2003",
                 "Seven Darlington transistor array, 500mA, 50V", "driver,darlington,relay,stepper", "uln2003a"},
            };
            for (const ic &c : list)
            {
                const package_info pkg = ic_package(c.package);
                part p;
                p.id = std::string(c.mpn);
                p.name = c.value == c.mpn ? std::string(c.mpn) : std::format("{} ({})", c.value, c.package);
                p.category = std::string(c.category);
                p.description = std::string(c.description);
                p.manufacturer = std::string(c.manufacturer);
                p.mpn = std::string(c.mpn);
                p.value = std::string(c.value);
                p.package = std::string(c.package == "SOIC-8W" ? "SOIC-8 (5.3mm)" : c.package);
                p.prefix = "U";
                p.symbol = std::string(c.symbol);
                p.footprint = std::string(pkg.footprint);
                p.model = std::string(pkg.model);
                if (!c.datasheet.empty())
                    p.datasheet = std::format("https://www.ti.com/lit/ds/symlink/{}.pdf", c.datasheet);
                std::string_view rest = c.keywords;
                while (!rest.empty())
                {
                    const std::size_t comma = rest.find(',');
                    p.keywords.emplace_back(rest.substr(0, comma));
                    if (comma == std::string_view::npos)
                        break;
                    rest.remove_prefix(comma + 1);
                }
                lib.parts.push_back(std::move(p));
            }
            return lib;
        }

        // ---- connectors --------------------------------------------------------

        library connectors()
        {
            library lib = make_library("volt-connectors", "Volt Connectors",
                                       "Pin headers and sockets, JST wire-to-board, USB, power and screw terminals.");
            const auto header = [&](std::string_view kind, int rows, int pins, std::string_view footprint_kind,
                                    std::string_view category) {
                part p;
                const int columns = pins / rows;
                p.id = std::format("{}_{}x{:02}_P2.54mm", kind, rows, columns);
                p.name = std::format("{} {}x{} 2.54mm", kind == "PinHeader" ? "Pin header" : "Pin socket", rows,
                                     columns);
                p.category = std::string(category);
                p.description = std::format("{}, {} row{}, {} pins, 2.54mm pitch, vertical, through hole",
                                            kind == "PinHeader" ? "Pin header" : "Female pin socket", rows == 1 ? "single" : "double",
                                            rows == 1 ? "" : "s", pins);
                p.value = std::format("{}x{}", rows, columns);
                p.package = std::format("{}x{:02} P2.54mm", rows, columns);
                p.prefix = "J";
                p.symbol = std::format("Conn_{:02}x{:02}", rows, columns);
                p.footprint = std::format("Connector_{}_2.54mm:{}_{}x{:02}_P2.54mm_Vertical", footprint_kind, kind,
                                          rows, columns);
                p.keywords = {"connector", "header", kind == "PinHeader" ? "male" : "female", "2.54mm", "0.1in", "tht"};
                p.parameters = {
                    param("Positions", std::to_string(pins)),
                    param("Rows", std::to_string(rows)),
                    param("Pitch", "2.54", "mm"),
                };
                lib.parts.push_back(std::move(p));
            };
            for (int n = 1; n <= 40; ++n)
                header("PinHeader", 1, n, "PinHeader", "Connectors/Pin Headers");
            for (int n = 2; n <= 40; ++n)
                header("PinHeader", 2, 2 * n, "PinHeader", "Connectors/Pin Headers");
            for (int n = 1; n <= 20; ++n)
                header("PinSocket", 1, n, "PinSocket", "Connectors/Pin Sockets");

            struct jst_series
            {
                std::string_view name;
                std::string_view pitch;
                std::string_view mpn_format; // {} is the number of positions
                std::string_view footprint;  // {} as well
                bool smd;
            };
            constexpr jst_series series[] = {
                {"PH", "2.00", "B{}B-PH-K-S", "Connector_JST:JST_PH_B{}B-PH-K_1x{:02}_P2.00mm_Vertical", false},
                {"XH", "2.50", "B{}B-XH-A", "Connector_JST:JST_XH_B{}B-XH-A_1x{:02}_P2.50mm_Vertical", false},
                {"SH", "1.00", "SM{:02}B-SRSS-TB", "Connector_JST:JST_SH_SM{:02}B-SRSS-TB_1x{:02}-1MP_P1.00mm_Horizontal",
                 true},
            };
            for (const jst_series &s : series)
            {
                for (int n = 2; n <= 10; ++n)
                {
                    part p;
                    p.mpn = std::vformat(s.mpn_format, std::make_format_args(n));
                    p.id = std::format("JST_{}_{}", s.name, p.mpn);
                    p.name = std::format("JST {} {} pins", s.name, n);
                    p.category = "Connectors/JST";
                    p.description = std::format("JST {} series wire-to-board header, {} positions, {}mm pitch, {}",
                                                s.name, n, s.pitch, s.smd ? "SMD, side entry" : "through hole, top entry");
                    p.manufacturer = "JST";
                    p.value = std::format("{}P", n);
                    p.package = std::format("JST {} 1x{:02}", s.name, n);
                    p.prefix = "J";
                    p.symbol = std::format("Conn_01x{:02}", n);
                    p.footprint = std::vformat(s.footprint, std::make_format_args(n, n));
                    p.keywords = {"connector", "jst", "wire to board", s.smd ? "smd" : "tht"};
                    p.parameters = {
                        param("Positions", std::to_string(n)),
                        param("Pitch", std::string(s.pitch), "mm"),
                    };
                    lib.parts.push_back(std::move(p));
                }
            }

            struct single
            {
                std::string_view id;
                std::string_view name;
                std::string_view category;
                std::string_view description;
                std::string_view manufacturer;
                std::string_view mpn;
                std::string_view package;
                std::string_view symbol;
                std::string_view footprint;
                std::string_view keywords;
            };
            constexpr single others[] = {
                {"USB_C_Receptacle_USB4105", "USB-C receptacle (USB 2.0)", "Connectors/USB",
                 "USB Type-C receptacle, USB 2.0, 16 pins, SMD, horizontal", "GCT", "USB4105-GF-A", "USB-C",
                 "USB_C_Receptacle_USB2.0", "Connector_USB:USB_C_Receptacle_GCT_USB4105-xx-A_16P_TopMnt_Horizontal",
                 "connector,usb,usb-c,type-c,smd"},
                {"USB_Micro_B_Molex_105017", "Micro-USB B receptacle", "Connectors/USB",
                 "Micro-USB type B receptacle, 5 pins, SMD, horizontal", "Molex", "1050170001", "Micro-USB",
                 "USB_B_Micro", "Connector_USB:USB_Micro-B_Molex_105017-0001", "connector,usb,micro-usb,smd"},
                {"BarrelJack_PJ-002A", "DC barrel jack 2.1mm", "Connectors/Power",
                 "DC power jack, 2.1mm center pin, 5.5mm barrel, through hole", "CUI Devices", "PJ-002A", "Barrel jack",
                 "Barrel_Jack", "Connector_BarrelJack:BarrelJack_CUI_PJ-002A_Horizontal",
                 "connector,power,barrel,dc jack,tht"},
                {"TerminalBlock_MKDS-1.5-2", "Screw terminal 2 pins 5.08mm", "Connectors/Terminal Blocks",
                 "PCB screw terminal block, 2 positions, 5.08mm pitch", "Phoenix Contact", "1729128", "MKDS 1,5/2-5,08",
                 "Conn_01x02", "TerminalBlock_Phoenix:TerminalBlock_Phoenix_MKDS-1,5-2-5.08_1x02_P5.08mm_Horizontal",
                 "connector,terminal,screw,tht"},
                {"TerminalBlock_MKDS-1.5-3", "Screw terminal 3 pins 5.08mm", "Connectors/Terminal Blocks",
                 "PCB screw terminal block, 3 positions, 5.08mm pitch", "Phoenix Contact", "1729131", "MKDS 1,5/3-5,08",
                 "Conn_01x03", "TerminalBlock_Phoenix:TerminalBlock_Phoenix_MKDS-1,5-3-5.08_1x03_P5.08mm_Horizontal",
                 "connector,terminal,screw,tht"},
            };
            for (const single &s : others)
            {
                part p;
                p.id = std::string(s.id);
                p.name = std::string(s.name);
                p.category = std::string(s.category);
                p.description = std::string(s.description);
                p.manufacturer = std::string(s.manufacturer);
                p.mpn = std::string(s.mpn);
                p.value = std::string(s.mpn);
                p.package = std::string(s.package);
                p.prefix = "J";
                p.symbol = std::string(s.symbol);
                p.footprint = std::string(s.footprint);
                std::string_view rest = s.keywords;
                while (!rest.empty())
                {
                    const std::size_t comma = rest.find(',');
                    p.keywords.emplace_back(rest.substr(0, comma));
                    if (comma == std::string_view::npos)
                        break;
                    rest.remove_prefix(comma + 1);
                }
                lib.parts.push_back(std::move(p));
            }
            return lib;
        }
    } // namespace

    namespace
    {
        // A part's id is its permanent identity and says nothing about the
        // part (<library/edit.hpp>), so the readable keys the functions
        // above build their parts under ("R_0603_10k_1%") do not go into
        // the files. They are what the id is made from, though: the same
        // library and key always give the same id, so that regenerating
        // the libraries, or adding parts to them, leaves every existing id
        // as it was. Changing a key gives the part a new identity: do not.
        std::string permanent_id(std::string_view library_name, std::string_view key)
        {
            constexpr std::string_view alphabet = "0123456789abcdefghjkmnpqrstvwxyz";
            std::uint64_t hash = 14695981039346656037ull; // FNV-1a
            const auto mix = [&](std::string_view text) {
                for (const char c : text)
                {
                    hash ^= static_cast<unsigned char>(c);
                    hash *= 1099511628211ull;
                }
            };
            mix(library_name);
            mix(":");
            mix(key);
            std::string id = "p-";
            for (int i = 0; i < 12; ++i, hash >>= 5)
                id += alphabet[hash & 31];
            return id;
        }

        library with_permanent_ids(library lib)
        {
            std::set<std::string> taken;
            for (part &p : lib.parts)
            {
                // Two keys that hash alike is next to impossible, and would
                // go unseen: step past it the same way every time.
                std::string key = p.id;
                std::string id = permanent_id(lib.name, key);
                while (!taken.insert(id).second)
                {
                    key += '+';
                    id = permanent_id(lib.name, key);
                }
                p.id = std::move(id);
            }
            return lib;
        }
    } // namespace

    std::vector<library> libraries()
    {
        std::vector<library> result;
        result.push_back(with_permanent_ids(resistors()));
        result.push_back(with_permanent_ids(capacitors()));
        result.push_back(with_permanent_ids(inductors()));
        result.push_back(with_permanent_ids(discretes()));
        result.push_back(with_permanent_ids(ics()));
        result.push_back(with_permanent_ids(connectors()));
        return result;
    }

} // namespace volt::library::seed
