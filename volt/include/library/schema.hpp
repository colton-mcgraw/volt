#pragma once

#include <library/part.hpp>

#include <json/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// -------------------------------------------------------------------------
// schema
// -------------------------------------------------------------------------
//
// What the parameters of a part mean. A part's parameters are free text
// (<library/part.hpp>), which is what lets a library say anything a
// datasheet does; the schema is the list of the names that mean something
// to Volt, so that a part can be checked against it and a simulation can
// ask for a parameter without both sides having to spell it alike:
//
//   parameters   "Resistance" is a quantity in Ω, also written "Ohms";
//                "Dielectric" is text
//   devices      a "resistor" needs a Resistance, and the DC operating
//                point is what reads it
//
// A simulation reads through the schema, by the names below:
//
//   const library::schema &s = library::schema::builtin();
//   s.number(part, library::names::resistance)   4700.0, from "4.7k" Ω,
//                                                "4k7", or "4.7" kΩ
//
// and says what it reads (schema::require), so that the check of a part can
// say what will not be found before anything is simulated:
//
//   s.check(part)
//     warning  "Resistence" is not a parameter Volt knows; did you mean
//              "Resistance"?
//     error    a resistor needs "Resistance": the DC operating point reads it
//
// Names are matched loosely: without regard to case, spaces, '_', '-' and
// '.', and through the aliases of a definition, so "forward_voltage", "Vf"
// and "Forward voltage" are one parameter. A name the schema does not know
// is not an error: the part keeps it and shows it, and the check notes that
// nothing reads it, or, when it is close to a known name, asks whether
// that one was meant.
//
// Some names are too short to mean one thing: "Voltage" is the voltage
// rating of a capacitor, and of a regulator it could be any of five. Such
// a short name belongs to a parameter for the devices it is unambiguous
// for, and nowhere else: on any other part the check says that the name
// does not say which, rather than take it for the rating.
//
// A quantity may be a typical value, limits, or both (parameter::value,
// min and max), with the condition they hold at. number() is the one
// figure to compute with: the typical value, or the middle of the limits
// when that is all there is; range() gives all three.
//
// A schema is a plain value; the built-in one is immutable and shared.
// Copy it to add definitions (a tool's own parameters, another simulation's
// requirements).

namespace volt::library
{

    // The built-in parameters and devices, by the names to ask for them.
    namespace names
    {
        inline constexpr std::string_view resistance = "Resistance";
        inline constexpr std::string_view capacitance = "Capacitance";
        inline constexpr std::string_view inductance = "Inductance";
        inline constexpr std::string_view tolerance = "Tolerance";
        inline constexpr std::string_view power_rating = "Power rating";
        inline constexpr std::string_view voltage_rating = "Voltage rating";
        inline constexpr std::string_view current_rating = "Current rating";
        inline constexpr std::string_view temperature_coefficient = "Temperature coefficient";
        inline constexpr std::string_view temperature_rating = "Temperature rating";
        inline constexpr std::string_view esr = "ESR";
        inline constexpr std::string_view dc_resistance = "DC resistance";
        inline constexpr std::string_view saturation_current = "Saturation current";
        inline constexpr std::string_view forward_voltage = "Forward voltage";
        inline constexpr std::string_view forward_current = "Forward current";
        inline constexpr std::string_view reverse_voltage = "Reverse voltage";
        inline constexpr std::string_view zener_voltage = "Zener voltage";
    } // namespace names

    namespace devices
    {
        inline constexpr std::string_view resistor = "resistor";
        inline constexpr std::string_view capacitor = "capacitor";
        inline constexpr std::string_view inductor = "inductor";
        inline constexpr std::string_view diode = "diode";
        inline constexpr std::string_view led = "led";
        inline constexpr std::string_view zener = "zener";
        inline constexpr std::string_view bjt = "bjt";
        inline constexpr std::string_view mosfet = "mosfet";
        inline constexpr std::string_view regulator = "regulator";
    } // namespace devices

    enum class value_kind : std::uint8_t
    {
        quantity, // a number in engineering notation, in the definition's unit
        text,     // anything, or one of the definition's choices
    };

    struct parameter_definition
    {
        std::string name; // as written in a library: "Resistance"
        value_kind kind = value_kind::quantity;
        // The unit a quantity is in: "Ω", "%", "mm"; empty for a count or a
        // ratio. A value may carry an SI prefix on its unit ("700" "mV"
        // for a quantity in V).
        std::string unit;
        // Units it is also given in and that are not `unit` with a prefix:
        // a tolerance is in % or, for a small capacitor, in F ("0.25" "pF").
        // Accepted by the check; number() has no value for them, since it
        // answers in `unit`.
        std::vector<std::string> other_units;
        std::vector<std::string> aliases; // other names for it: "Ohms"
        // Names that are this parameter only on a part of one of these
        // devices: "Voltage", for a resistor, capacitor or inductor.
        struct short_name
        {
            std::string name;
            std::vector<std::string> devices; // device ids

            bool operator==(const short_name &) const = default;
        };
        std::vector<short_name> short_names;
        // For text: the values it can have, compared loosely; empty for any.
        std::vector<std::string> choices;
        std::string description;

        bool operator==(const parameter_definition &) const = default;
    };

    // A parameter a device has, and who wants it.
    struct device_parameter
    {
        std::string name; // a parameter definition's name
        // Whether a part of this device is unusable without it (an error to
        // leave out) or merely better with it.
        bool required = true;
        // What reads it, as a phrase for messages: "the DC operating
        // point"; empty when it is simply what the device is.
        std::string used_by;

        bool operator==(const device_parameter &) const = default;
    };

    struct device_definition
    {
        std::string id;   // what part::device holds: "resistor"
        std::string name; // for people: "Resistor"
        std::vector<std::string> aliases;
        std::vector<device_parameter> parameters;
        std::string description;

        bool operator==(const device_definition &) const = default;
    };

    enum class severity : std::uint8_t
    {
        note,    // worth knowing; nothing is wrong
        warning, // probably a mistake
        error,   // a simulation of this part will not work
    };

    std::string_view describe(severity level) noexcept; // "note", "warning", "error"

    enum class issue_code : std::uint8_t
    {
        unknown_parameter,    // not in the schema, and not close to anything in it
        misspelled_parameter, // not in the schema, but close to a name that is
        ambiguous_parameter,  // a short name that does not say which, on this device
        duplicate_parameter,  // the same parameter twice
        missing_value,        // a parameter with no value
        not_a_number,         // a quantity whose value is not a number
        wrong_unit,           // a quantity in a unit that is not the definition's
        bad_range,            // limits the wrong way round, or a value outside them
        unknown_choice,       // text that is not one of the definition's choices
        unknown_device,       // part::device is not in the schema
        missing_parameter,    // the device needs a parameter the part lacks
    };

    // "unknown-parameter", ...: stable, for tools.
    std::string_view describe(issue_code code) noexcept;

    struct issue
    {
        severity level = severity::note;
        issue_code code = issue_code::unknown_parameter;
        // The parameter it is about, as the part writes it (as the schema
        // does, for a missing one); empty for an issue with the device.
        std::string parameter;
        std::string message;
        // What was probably meant, when there is a guess: a parameter's or
        // a device's name.
        std::string suggestion;

        bool operator==(const issue &) const = default;
    };

    // The worst level among the issues; note for none.
    severity worst(std::span<const issue> issues) noexcept;

    // A quantity's figures in its definition's unit, each when it is given
    // and is a number.
    struct quantity_range
    {
        std::optional<double> min;
        std::optional<double> typical;
        std::optional<double> max;

        bool operator==(const quantity_range &) const = default;
    };

    struct parameter_definition;

    // How a name is read (schema::resolve): for a tool that writes parts to
    // ask before it writes them.
    struct resolution
    {
        enum class kind : std::uint8_t
        {
            name,       // the parameter's own name
            alias,      // one of its aliases
            short_name, // a short name, which is this parameter on this device
            ambiguous,  // a short name that is not, on this device: see candidates
            unknown,    // not known: candidates are the names it may be a misspelling of
        };

        kind how = kind::unknown;
        // The parameter it is; nullptr when ambiguous or unknown.
        const parameter_definition *definition = nullptr;
        std::vector<std::string> candidates;
    };

    std::string_view describe(resolution::kind how) noexcept; // "name", "alias", "short-name", ...

    class schema
    {
    public:
        schema() = default;

        // The parameters and devices Volt knows itself.
        static const schema &builtin();

        // ---- definitions ---------------------------------------------------

        // Adds a definition, replacing the one of the same name (or id).
        void add(parameter_definition definition);
        void add(device_definition definition);

        // Says that a device has a parameter and who reads it: what a
        // simulation calls for each parameter it will ask for. Adds to what
        // is known already (a parameter optional so far becomes required;
        // never the other way). False, and nothing changes, when the
        // device or the parameter is not defined.
        bool require(std::string_view device, std::string_view parameter, std::string_view used_by,
                     bool required = true);

        std::span<const parameter_definition> parameters() const noexcept { return m_parameters; }
        std::span<const device_definition> devices() const noexcept { return m_devices; }

        // By name or alias, loosely (see above); nullptr when not known. A
        // short name is found only with a device it belongs to.
        const parameter_definition *find(std::string_view name, std::string_view device = {}) const;
        const device_definition *find_device(std::string_view id) const;

        // How a name would be read on a part of that device (or of none),
        // with what it could be when it is not read as anything.
        resolution resolve(std::string_view name, std::string_view device = {}) const;

        // The known parameter names closest to an unknown one, nearest
        // first: its probable spellings. Empty when nothing is close.
        std::vector<std::string> suggest(std::string_view name, std::size_t limit = 3) const;
        std::vector<std::string> suggest_device(std::string_view id, std::size_t limit = 3) const;

        // ---- reading a part ------------------------------------------------

        // The part's parameter that is this one, however the part spells
        // it: the first of them, or nullptr. A name the schema does not
        // know is matched loosely against the part's own names.
        const parameter *find_parameter(const part &p, std::string_view name) const;

        // Its value in the definition's unit ("4.7k" Ω and "4.7" kΩ are
        // both 4700; "1" % is 1): what a simulation computes with. For a
        // parameter given as limits alone, the middle of them. Empty when
        // the part has no such parameter, or it is not a number, or it has
        // one limit and no value.
        std::optional<double> number(const part &p, std::string_view name) const;
        // The same for one parameter, of a part of that device.
        std::optional<double> number(const parameter &q, std::string_view device = {}) const;

        // The limits and the typical value, each when given.
        quantity_range range(const part &p, std::string_view name) const;
        quantity_range range(const parameter &q, std::string_view device = {}) const;

        // ---- checking a part -----------------------------------------------

        // Everything about the part's parameters and device a simulation
        // would stumble on, in the order of the parameters, then the
        // device's. Empty for a part that is fine.
        std::vector<issue> check(const part &p) const;

        // Rewrites what can be without changing what it means: a known
        // parameter gets its definition's name ("ohms" becomes
        // "Resistance"; "Voltage" becomes "Voltage rating" on a capacitor)
        // and unit ("700" "mV" becomes "700m" "V"), a device its id. Returns how many things changed. Unknown names are left
        // alone.
        std::size_t normalize(part &p) const;

    private:
        struct string_hash
        {
            using is_transparent = void;
            std::size_t operator()(std::string_view text) const noexcept { return std::hash<std::string_view>{}(text); }
        };
        using name_map = std::unordered_map<std::string, std::uint32_t, string_hash, std::equal_to<>>;

        void index();

        std::vector<parameter_definition> m_parameters;
        std::vector<device_definition> m_devices;
        // Folded names and aliases to positions in the vectors.
        name_map m_parameter_names;
        name_map m_device_names;
        // Folded short names to the parameters they can be, each with the
        // folded ids of the devices it is that parameter for.
        struct short_use
        {
            std::uint32_t parameter = 0;
            std::vector<std::string> devices;
        };
        std::unordered_map<std::string, std::vector<short_use>, string_hash, std::equal_to<>> m_short_names;

        // The folded id of the device a part's `device` text names, or
        // empty.
        std::string device_key(std::string_view device) const;
        const parameter_definition *find_short(std::string_view folded_name, std::string_view device) const;
        std::vector<std::string> meanings(std::string_view folded_name) const;
    };

    // The definitions as JSON, for tools that write parts to see what Volt
    // will understand:
    //
    //   {"parameters": [{"name": "Resistance", "kind": "quantity", "unit": "Ω",
    //                    "aliases": ["Ohms"], "description": "...",
    //                    "short_names": [{"name": "...", "devices": ["..."]}]}],
    //    "devices": [{"id": "resistor", "name": "Resistor",
    //                 "parameters": [{"name": "Resistance", "required": true,
    //                                 "used_by": "the DC operating point"}]}]}
    json::value schema_to_json(const schema &s);

    // An issue as JSON: {"level", "code", "parameter", "message",
    // "suggestion"}, empty ones left out.
    json::value issue_to_json(const issue &i);

} // namespace volt::library
