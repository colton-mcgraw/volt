#pragma once

#include <json/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// -------------------------------------------------------------------------
// part
// -------------------------------------------------------------------------
//
// One orderable component as a library describes it: what it is, who makes
// it, and which symbol, footprint and 3D model draw it. There is no
// schematic or board document model yet, so those three are references by
// name or path, resolved later by whatever draws them:
//
//   symbol     a symbol's name, e.g. "R" or "OpAmp_Dual"
//   footprint  "<footprint library>:<footprint>", e.g.
//              "Resistor_SMD:R_0603_1608Metric", the KiCad convention
//   model      the name of a file the part's library carries itself
//              (library::files, see <library/library.hpp>), or else a path
//              relative to the models directory (assets/models); UTF-8
//              with forward slashes, e.g. "parts/R_1206_3216Metric.step"
//
// What a simulation reads of a part is its device ("resistor") and its
// parameters; <library/schema.hpp> knows the names and checks them.
//
// When symbols and footprints gain geometry it goes in new fields; files
// carry a format version, and fields this version does not know are kept in
// `extra` and written back, so an older Volt does not lose them.

namespace volt::library
{

    // A free-form property of a part, for parametric data: resistance,
    // tolerance, voltage rating, ... The value is kept as written, with its
    // unit beside it, so a library says "4.7k" and "Ω" rather than
    // 4700.000000000001.
    struct parameter
    {
        std::string name;  // "Resistance"
        std::string value; // "4.7k", "1", "X7R"
        std::string unit;  // "Ω", "%", "V"; empty for none

        // Fields this version does not know, kept.
        json::object extra;

        // A datasheet seldom gives one number. `value` is the typical (or
        // nominal) one, and may be empty when only limits are given; `min`
        // and `max` are the limits, written as `value` is and in the same
        // unit; `condition` is what they were measured at, as free text:
        //
        //   {"name": "Forward voltage", "value": "2.0", "min": "1.8",
        //    "max": "2.4", "unit": "V", "condition": "If = 20 mA"}
        //
        // (After `extra` so that {name, value, unit, extra} still makes a
        // parameter.)
        std::string min = {};
        std::string max = {};
        std::string condition = {};

        // The value as a number in plain SI units, for a value in
        // engineering notation ("4.7k", "4k7", "100n", "1e-7", "100 nF" with
        // unit "F"): 4700, 1e-7. Empty for text such as "X7R".
        std::optional<double> number() const noexcept;

        // As shown: "4.7kΩ", "1%", "X7R"; with limits "2.0V (1.8 to 2.4)",
        // or "1.8 to 2.4V" without a typical value, "up to 2.4V" with one
        // limit alone; and the condition after it: "1V at If = 50 mA".
        std::string text() const;

        // A parameter for an electrical quantity, its value written in
        // engineering notation (math::electrical::format_engineering):
        // quantity("Capacitance", 1e-7, "F") is {"Capacitance", "100n", "F"}.
        static parameter quantity(std::string name, double value, std::string unit, int significant_digits = 4);

        bool operator==(const parameter &) const = default;
    };

    // Where a copied part came from, so that a copy (in a project library,
    // or a user library built from shipped parts) can be compared with its
    // source later: has the source changed since, has the copy?
    struct part_origin
    {
        std::string library; // the source library's name
        std::string part;    // the part's id there
        std::string version; // the source library's version when copied
        std::string hash;    // part_hash() of the part as copied

        bool operator==(const part_origin &) const = default;
    };

    // Where a part can be bought, and what it is called there.
    struct supplier
    {
        std::string name; // "LCSC", "Digi-Key"
        std::string sku;  // the supplier's own order code: "C25804"
        std::string url;  // the part's page there

        json::object extra;

        bool operator==(const supplier &) const = default;
    };

    struct part
    {
        // Stable and unique within its library; with the library's name it
        // makes the fully qualified id "library:part". Any text without ':',
        // control characters, or space at either end (see valid_id).
        std::string id;
        std::string name;
        // A path of categories from the most general: "Passives/Resistors".
        std::string category;
        std::string description;
        std::string manufacturer;
        std::string mpn; // manufacturer part number
        std::string value;
        std::string package;
        // The reference designator's letters: "R", "C", "U".
        std::string prefix;
        // What the part is to a simulation: "resistor", "capacitor",
        // "diode", ... (a device of <library/schema.hpp>), which decides
        // the parameters a simulation looks for. Empty for a part no
        // simulation models (a connector, a mounting hole).
        std::string device;
        std::string symbol;
        std::string footprint;
        std::string model;
        std::string datasheet; // a URL
        std::vector<std::string> keywords;
        std::vector<parameter> parameters; // in order
        std::vector<supplier> suppliers;   // in order of preference
        std::optional<part_origin> origin;

        // Fields this version of Volt does not know, in file order, kept so
        // that saving writes them back.
        json::object extra;

        // The first parameter of that name (exact match), or nullptr.
        const parameter *find_parameter(std::string_view parameter_name) const noexcept;

        bool operator==(const part &) const = default;
    };

    // Whether text can be a part id or a library name: not empty, no ':', no
    // control characters, no space at the start or end.
    bool valid_id(std::string_view text) noexcept;

    // A category path tidied: segments trimmed, empty ones dropped, joined
    // with '/'. " Passives / Resistors/ " is "Passives/Resistors".
    std::string normalize_category(std::string_view category);

    // A 64-bit FNV-1a hash of the part's content, as 16 hex digits: every
    // field, extra ones included, except its id and origin (a copy that was
    // renamed, or is linked, is still the same part). Equal parts have equal
    // hashes, in any process and on any platform.
    std::string part_hash(const part &p);

    // The part as the JSON object the files hold, and back. Reading reports
    // problems with known fields (a number where a string belongs) to
    // `problems` and leaves the field empty; unknown fields go to extra.
    // The id and name are not checked here; library loading does that.
    json::value part_to_json(const part &p);
    part part_from_json(const json::object &object, std::vector<std::string> &problems);

} // namespace volt::library
