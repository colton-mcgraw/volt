// The schema: what parameter names mean, reading a part through it, and
// the check that says what a simulation will not find.

#include "library_test_support.hpp"

#include <library/schema.hpp>

#include <algorithm>
#include <cmath>

using namespace volt;
using tests::make_part;

namespace
{
    const library::schema &rules = library::schema::builtin();

    bool near(std::optional<double> value, double expected)
    {
        return value && std::abs(*value - expected) <= 1e-12 * std::abs(expected);
    }

    library::part resistor(std::vector<library::parameter> parameters)
    {
        library::part p = make_part("R1", "10k", "Passives/Resistors", "10k", "0603");
        p.device = "resistor";
        p.parameters = std::move(parameters);
        return p;
    }

    // The issue of that code, or nullptr.
    const library::issue *find(const std::vector<library::issue> &issues, library::issue_code code)
    {
        const auto it = std::ranges::find_if(issues, [&](const library::issue &i) { return i.code == code; });
        return it == issues.end() ? nullptr : &*it;
    }

    void names()
    {
        // By name, by alias, and loosely.
        const library::parameter_definition *r = rules.find("Resistance");
        CHECK(r != nullptr && r->unit == "\xCE\xA9");
        CHECK(rules.find("resistance") == r);
        CHECK(rules.find("RESISTANCE ") == r);
        CHECK(rules.find("Ohms") == r);
        CHECK(rules.find("Forward voltage") == rules.find("forward_voltage"));
        CHECK(rules.find("Vf") == rules.find(library::names::forward_voltage));
        // A short name is a parameter only on the devices it is plain for.
        CHECK(rules.find("Voltage") == nullptr);
        CHECK(rules.find("Voltage", "capacitor") == rules.find(library::names::voltage_rating));
        CHECK(rules.find("voltage", "Capacitor") == rules.find(library::names::voltage_rating));
        CHECK(rules.find("Voltage", "regulator") == nullptr);
        CHECK(rules.find("Power", "resistor") == rules.find(library::names::power_rating));
        CHECK(rules.find("Power", "capacitor") == nullptr);
        CHECK(rules.find("Rated voltage") == rules.find(library::names::voltage_rating));

        // How a name is read, for tools.
        CHECK(rules.resolve("Resistance").how == library::resolution::kind::name);
        CHECK(rules.resolve("ohms").how == library::resolution::kind::alias);
        CHECK(rules.resolve("ohms").definition == r);
        CHECK(rules.resolve("Voltage", "inductor").how == library::resolution::kind::short_name);
        {
            const library::resolution unsure = rules.resolve("Voltage", "diode");
            CHECK(unsure.how == library::resolution::kind::ambiguous && unsure.definition == nullptr);
            CHECK(!unsure.candidates.empty() && unsure.candidates.front() == "Voltage rating");
            CHECK(std::ranges::find(unsure.candidates, "Forward voltage") != unsure.candidates.end());
            CHECK(std::ranges::find(unsure.candidates, "Forward current") == unsure.candidates.end());
            const library::resolution wrong = rules.resolve("Resistence");
            CHECK(wrong.how == library::resolution::kind::unknown &&
                  wrong.candidates == std::vector<std::string>{"Resistance"});
            CHECK(rules.resolve("Lead finish").candidates.empty());
        }
        CHECK(rules.find("Resistence") == nullptr);
        CHECK(rules.find("") == nullptr);

        // Every name the header promises is defined.
        for (const std::string_view name :
             {library::names::resistance, library::names::capacitance, library::names::inductance,
              library::names::tolerance, library::names::power_rating, library::names::voltage_rating,
              library::names::current_rating, library::names::temperature_coefficient,
              library::names::temperature_rating, library::names::esr, library::names::dc_resistance,
              library::names::saturation_current, library::names::forward_voltage, library::names::forward_current,
              library::names::reverse_voltage, library::names::zener_voltage})
            CHECK(rules.find(name) != nullptr && rules.find(name)->name == name);
        for (const std::string_view id :
             {library::devices::resistor, library::devices::capacitor, library::devices::inductor,
              library::devices::diode, library::devices::led, library::devices::zener, library::devices::bjt,
              library::devices::mosfet})
            CHECK(rules.find_device(id) != nullptr && rules.find_device(id)->id == id);
        CHECK(rules.find_device("Resistor") == rules.find_device("resistor"));

        // What a device asks for is defined.
        for (const library::device_definition &d : rules.devices())
            for (const library::device_parameter &q : d.parameters)
                CHECK(rules.find(q.name) != nullptr);

        // Suggestions: typing mistakes, nearest first; nothing for names
        // that are simply something else.
        CHECK(rules.suggest("Resistence") == std::vector<std::string>{"Resistance"});
        CHECK(rules.suggest("Capacitence") == std::vector<std::string>{"Capacitance"});
        CHECK(rules.suggest("Inductnace") == std::vector<std::string>{"Inductance"}); // a swap
        CHECK(rules.suggest("forward_votage") == std::vector<std::string>{"Forward voltage"});
        CHECK(rules.suggest("Lead finish").empty());
        CHECK(rules.suggest("Q").empty());
        CHECK(rules.suggest_device("resistr") == std::vector<std::string>{"resistor"});
        CHECK(rules.suggest_device("relay").empty());
    }

    void numbers()
    {
        const library::part p = resistor({{"ohms", "4k7", "", {}},
                                          {"Tolerance", "1", "%", {}},
                                          {"Power", "62.5", "mW", {}},
                                          {"Note", "matched", "", {}}});
        // However the part spells it.
        CHECK(rules.find_parameter(p, "Resistance") == &p.parameters[0]);
        CHECK(rules.find_parameter(p, library::names::power_rating) == &p.parameters[2]);
        CHECK(rules.find_parameter(p, "note") == &p.parameters[3]);
        CHECK(rules.find_parameter(p, "Capacitance") == nullptr);

        CHECK(near(rules.number(p, library::names::resistance), 4700.0));
        CHECK(near(rules.number(p, library::names::tolerance), 1.0));
        // The unit's prefix counts: 62.5 mW is 0.0625 W.
        CHECK(near(rules.number(p, library::names::power_rating), 0.0625));
        CHECK(!rules.number(p, "Note"));
        CHECK(!rules.number(p, library::names::capacitance));

        const auto value = [&](std::string text, std::string unit) {
            return rules.number(library::parameter{"Resistance", std::move(text), std::move(unit), {}});
        };
        CHECK(near(value("4.7k", "\xCE\xA9"), 4700.0));
        CHECK(near(value("4.7", "k\xCE\xA9"), 4700.0));
        CHECK(near(value("4.7", "kOhm"), 4700.0));
        CHECK(near(value("4.7 k\xCE\xA9", ""), 4700.0));
        CHECK(near(value("47R", ""), 47.0));
        CHECK(near(value("2M2", "ohm"), 2.2e6));
        CHECK(value("0", "\xCE\xA9") == 0.0);
        CHECK(!value("4.7k", "k\xCE\xA9")); // two prefixes
        CHECK(!value("4.7", "V"));          // another unit
        CHECK(!value("lots", "\xCE\xA9"));
        CHECK(near(rules.number(library::parameter{"Capacitance", "100", "nF", {}}), 1e-7));
        CHECK(near(rules.number(library::parameter{"Capacitance", "4.7", "\xC2\xB5" "F", {}}), 4.7e-6));
        CHECK(near(rules.number(library::parameter{"Temperature", "105", "\xC2\xB0" "C", {}}), 105.0));
        CHECK(near(rules.number(library::parameter{"Pitch", "2.54", "mm", {}}), 2.54));
        CHECK(near(rules.number(library::parameter{"Positions", "8", "", {}}), 8.0));
        // A tolerance in farads is fine, but it is not a percentage.
        CHECK(!rules.number(library::parameter{"Tolerance", "0.25", "pF", {}}));
        // Unknown to the schema: as the parameter reads itself.
        CHECK(near(rules.number(library::parameter{"Ripple current", "1.2", "A", {}}), 1.2));

        // Limits: the typical value is the number; without one, the middle
        // of the limits; one limit alone is not a number to compute with.
        library::parameter vf{"Vf", "2.0", "V", {}};
        vf.min = "1800";
        vf.max = "2400";
        vf.unit = "mV";
        vf.value = "2000";
        library::quantity_range r = rules.range(vf);
        CHECK(near(r.min, 1.8) && near(r.typical, 2.0) && near(r.max, 2.4));
        CHECK(near(rules.number(vf), 2.0));
        vf.value.clear();
        CHECK(near(rules.number(vf), 2.1));
        CHECK(!rules.range(vf).typical);
        vf.min.clear();
        CHECK(!rules.number(vf) && near(rules.range(vf).max, 2.4));
        library::part led = make_part("D1", "LED", "Optoelectronics");
        led.device = "led";
        led.parameters = {vf};
        CHECK(near(rules.range(led, "Forward voltage").max, 2.4));
        CHECK(rules.range(led, "Forward current") == library::quantity_range{});
    }

    void checks()
    {
        // A part that is fine.
        CHECK(rules.check(resistor({library::parameter::quantity("Resistance", 1e4, "\xCE\xA9"),
                                    {"Tolerance", "1", "%", {}},
                                    {"voltage", "75", "V", {}},
                                    {"Series", "E96", "", {}}}))
                  .empty());
        // No device, no parameters: nothing to say.
        CHECK(rules.check(make_part("J1", "Header", "Connectors")).empty());

        // The misspelling: a warning that names what was meant, and, since
        // the resistance is then not found, the error that says who needs it.
        {
            const auto issues = rules.check(resistor({{"Resistence", "10k", "\xCE\xA9", {}}}));
            const library::issue *misspelled = find(issues, library::issue_code::misspelled_parameter);
            CHECK(misspelled != nullptr);
            if (misspelled != nullptr)
            {
                CHECK(misspelled->level == library::severity::warning);
                CHECK(misspelled->parameter == "Resistence");
                CHECK(misspelled->suggestion == "Resistance");
                CHECK(misspelled->message.find("did you mean \"Resistance\"") != std::string::npos);
            }
            const library::issue *missing = find(issues, library::issue_code::missing_parameter);
            CHECK(missing != nullptr);
            if (missing != nullptr)
            {
                CHECK(missing->level == library::severity::error);
                CHECK(missing->parameter == "Resistance");
                CHECK(missing->message.find("the DC operating point") != std::string::npos);
            }
            CHECK(library::worst(issues) == library::severity::error);
        }

        // A name of the part's own: kept, and noted.
        {
            const auto issues = rules.check(resistor(
                {library::parameter::quantity("Resistance", 1e4, "\xCE\xA9"), {"Lead finish", "Sn", "", {}}}));
            CHECK(issues.size() == 1);
            CHECK(find(issues, library::issue_code::unknown_parameter) != nullptr);
            CHECK(library::worst(issues) == library::severity::note);
        }

        // Values.
        {
            const auto issues = rules.check(resistor({{"Resistance", "ten k", "\xCE\xA9", {}}}));
            const library::issue *bad = find(issues, library::issue_code::not_a_number);
            CHECK(bad != nullptr && bad->level == library::severity::error);
        }
        {
            const auto issues = rules.check(resistor({{"Resistance", "10k", "V", {}}}));
            const library::issue *unit = find(issues, library::issue_code::wrong_unit);
            CHECK(unit != nullptr && unit->level == library::severity::error && unit->suggestion == "\xCE\xA9");
        }
        CHECK(find(rules.check(resistor({{"Resistance", " ", "\xCE\xA9", {}}})), library::issue_code::missing_value) !=
              nullptr);
        // A tolerance in the value's own unit is not a wrong unit.
        CHECK(rules.check(make_part("C1", "1p", "Passives", "1p")).empty());
        {
            library::part c = make_part("C1", "1p", "Passives", "1p");
            c.device = "capacitor";
            c.parameters = {library::parameter::quantity("Capacitance", 1e-12, "F"), {"Tolerance", "0.25", "pF", {}}};
            CHECK(rules.check(c).empty());
        }

        // A short name where it does not say which: a warning that lists
        // what it could be, on a part of another device and on one of none.
        {
            library::part u = make_part("U1", "LDO", "ICs");
            u.device = "regulator";
            u.parameters = {{"Voltage", "3.3", "V", {}}};
            const auto issues = rules.check(u);
            const library::issue *unsure = find(issues, library::issue_code::ambiguous_parameter);
            CHECK(issues.size() == 1 && unsure != nullptr && unsure->level == library::severity::warning);
            CHECK(unsure != nullptr && unsure->message.find("\"Output voltage\"") != std::string::npos);
            u.device.clear();
            CHECK(find(rules.check(u), library::issue_code::ambiguous_parameter) != nullptr);
            u.device = "capacitor";
            u.parameters.push_back(library::parameter::quantity("Capacitance", 1e-6, "F"));
            CHECK(rules.check(u).empty());
        }

        // Text with a list of values: one that is not on it.
        {
            library::part q = make_part("Q1", "BC847", "Discretes");
            q.device = "bjt";
            q.parameters = {{"Type", "npn", "", {}}};
            CHECK(rules.check(q).empty());
            q.parameters = {{"Type", "Thick film", "", {}}};
            CHECK(find(rules.check(q), library::issue_code::unknown_choice) != nullptr);
        }

        // Limits: they stand in for a value, must be numbers, and must be
        // the right way round with the value between them.
        {
            library::part led = make_part("D1", "LED", "Optoelectronics");
            led.device = "led";
            library::parameter vf{"Forward voltage", "", "V", {}};
            vf.min = "1.8";
            vf.max = "2.4";
            vf.condition = "If = 20 mA";
            led.parameters = {vf};
            CHECK(rules.check(led).empty()); // required, and there
            led.parameters[0].max = "lots";
            const auto bad = rules.check(led);
            CHECK(bad.size() == 1 && bad[0].code == library::issue_code::not_a_number);
            CHECK(!bad.empty() && bad[0].message.find("the maximum of") == 0);
            led.parameters[0].max = "1.2";
            CHECK(find(rules.check(led), library::issue_code::bad_range) != nullptr);
            led.parameters[0].max = "2.4";
            led.parameters[0].value = "3.0";
            CHECK(find(rules.check(led), library::issue_code::bad_range) != nullptr);
            led.parameters[0].value = "2.0";
            CHECK(rules.check(led).empty());
        }

        // The same parameter twice, under two names.
        {
            const auto issues = rules.check(resistor({{"Resistance", "10k", "\xCE\xA9", {}}, {"Ohms", "10k", "", {}}}));
            CHECK(issues.size() == 1 && issues[0].code == library::issue_code::duplicate_parameter);
        }

        // Devices.
        {
            library::part p = resistor({library::parameter::quantity("Resistance", 1e4, "\xCE\xA9")});
            p.device = "resistr";
            const auto issues = rules.check(p);
            CHECK(issues.size() == 1 && issues[0].code == library::issue_code::unknown_device);
            CHECK(!issues.empty() && issues[0].suggestion == "resistor");
        }
        {
            library::part led = make_part("D1", "Red LED", "Optoelectronics");
            led.device = "LED"; // loosely
            CHECK(find(rules.check(led), library::issue_code::missing_parameter) != nullptr);
            led.parameters = {{"Vf", "2.0", "V", {}}};
            CHECK(rules.check(led).empty());
        }

        // Stable names for tools.
        CHECK(library::describe(library::issue_code::misspelled_parameter) == "misspelled-parameter");
        CHECK(library::describe(library::severity::warning) == "warning");
        const json::value as_json = library::issue_to_json(
            {library::severity::error, library::issue_code::missing_parameter, "Resistance", "gone", {}});
        CHECK(as_json.at("level").string_or() == "error");
        CHECK(as_json.at("code").string_or() == "missing-parameter");
        CHECK(as_json.find("suggestion") == nullptr);
    }

    void normalizing()
    {
        library::part p = resistor({{"ohms", "4.7", "k\xCE\xA9", {}},
                                    {"power", "62.5", "mW", {}},
                                    {"Tolerance", "1", "%", {}},
                                    {"Lead finish", "Sn", "", {}}});
        p.device = "Resistor";
        const std::size_t changes = rules.normalize(p);
        CHECK(changes == 5);
        CHECK(p.parameters[0] == (library::parameter{"Resistance", "4.7k", "\xCE\xA9", {}}));
        CHECK(p.parameters[1] == (library::parameter{"Power rating", "62.5m", "W", {}}));
        CHECK(p.parameters[2] == (library::parameter{"Tolerance", "1", "%", {}}));
        CHECK(p.parameters[3].name == "Lead finish");
        CHECK(p.device == "resistor");

        // A short name gets the full one where it is one, and is left
        // alone where it is not; limits move with the value.
        library::part c = make_part("C1", "100n", "Passives");
        c.device = "Capacitor";
        library::parameter rating{"Voltage", "", "kV", {}};
        rating.min = "1";
        rating.max = "2";
        c.parameters = {rating};
        CHECK(rules.normalize(c) == 3);
        CHECK(c.parameters[0].name == "Voltage rating" && c.parameters[0].unit == "V");
        CHECK(c.parameters[0].min == "1k" && c.parameters[0].max == "2k" && c.parameters[0].value.empty());
        library::part u = make_part("U1", "LDO", "ICs");
        u.device = "regulator";
        u.parameters = {{"Voltage", "3.3", "V", {}}};
        CHECK(rules.normalize(u) == 0 && u.parameters[0].name == "Voltage");
        CHECK(near(rules.number(p, "Resistance"), 4700.0));
        // And again changes nothing.
        CHECK(rules.normalize(p) == 0);
    }

    void extending()
    {
        // A copy takes definitions of its own; the built-in one stays.
        library::schema mine = rules;
        library::parameter_definition ripple;
        ripple.name = "Ripple current";
        ripple.unit = "A";
        ripple.aliases = {"Irms"};
        mine.add(ripple);
        CHECK(mine.find("irms") != nullptr && rules.find("irms") == nullptr);
        CHECK(near(mine.number(library::parameter{"Irms", "850", "mA", {}}), 0.85));

        // A simulation says what it reads.
        CHECK(mine.require("capacitor", "Ripple current", "the thermal analysis"));
        CHECK(!mine.require("capacitor", "No such thing", "x"));
        CHECK(!mine.require("no-such-device", "Resistance", "x"));
        library::part c = make_part("C1", "100u", "Passives", "100u");
        c.device = "capacitor";
        c.parameters = {library::parameter::quantity("Capacitance", 1e-4, "F")};
        CHECK(rules.check(c).empty());
        const auto issues = mine.check(c);
        CHECK(issues.size() == 1 && issues[0].code == library::issue_code::missing_parameter);
        CHECK(!issues.empty() && issues[0].message.find("the thermal analysis") != std::string::npos);

        // An optional parameter becomes required, and never the other way.
        CHECK(mine.require("resistor", "Tolerance", "the Monte Carlo run"));
        CHECK(mine.require("resistor", "Resistance", "the Monte Carlo run", false));
        const library::device_definition *r = mine.find_device("resistor");
        CHECK(r != nullptr);
        if (r != nullptr)
            for (const library::device_parameter &q : r->parameters)
            {
                if (q.name == "Tolerance")
                    CHECK(q.required && q.used_by == "the Monte Carlo run");
                if (q.name == "Resistance")
                    CHECK(q.required && q.used_by == "the DC operating point and the Monte Carlo run");
            }

        // The definitions as JSON list every one.
        const json::value listed = library::schema_to_json(rules);
        CHECK(listed.at("parameters").size() == rules.parameters().size());
        CHECK(listed.at("devices").size() == rules.devices().size());
        CHECK(listed.at("parameters").at(0).at("name").string_or() == "Resistance");
    }
} // namespace

int main()
{
    names();
    numbers();
    checks();
    normalizing();
    extending();
    return volt::tests::result("schema");
}
