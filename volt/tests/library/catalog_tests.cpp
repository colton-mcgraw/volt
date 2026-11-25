// The catalog: lookup by qualified id, the category tree, search (ranking,
// filters, quantities), and discovering and loading libraries from
// directories.

#include "library_test_support.hpp"

#include <algorithm>
#include <atomic>
#include <format>
#include <thread>

using namespace volt;
using tests::make_part;

namespace
{
    library::catalog sample_catalog()
    {
        library::library alpha = tests::make_library("alpha", "1.0");
        auto r10k = make_part("R_10k_0603", "10k\xCE\xA9 1% 0603", "Passives/Resistors", "10k", "0603");
        r10k.mpn = "RC0603FR-0710KL";
        r10k.manufacturer = "Yageo";
        r10k.keywords = {"resistor", "smd"};
        r10k.description = "Thick film chip resistor";
        alpha.parts.push_back(r10k);                                                                            // 0
        alpha.parts.push_back(make_part("R_100k_0603", "100k\xCE\xA9 1% 0603", "Passives/Resistors", "100k", "0603")); // 1
        alpha.parts.push_back(make_part("R_10k_0805", "10k\xCE\xA9 1% 0805", "Passives/Resistors", "10k", "0805"));    // 2
        alpha.parts.push_back(make_part("R_4k7_0603", "4.7k\xCE\xA9 0603", "Passives/Resistors", "4.7k", "0603"));     // 3
        alpha.parts.push_back(make_part("C_100n_0603", "100nF 0603", "Passives/Capacitors", "100nF", "0603"));         // 4
        auto lm358 = make_part("LM358", "LM358", "ICs/Amplifiers", "LM358", "SOIC-8");
        lm358.mpn = "LM358DR";
        lm358.description = "Dual op-amp";
        alpha.parts.push_back(lm358);                                                                           // 5
        alpha.parts.push_back(make_part("LM3580", "LM3580", "ICs/Amplifiers", "LM3580", "SOIC-8"));             // 6
        auto clone = make_part("XY1", "XY1 amplifier", "ICs/Amplifiers", "XY1", "SOIC-8");
        clone.description = "Pin compatible with the lm358";
        alpha.parts.push_back(clone);                                                                           // 7
        alpha.parts.push_back(make_part("L_10u", "10\xC2\xB5H inductor", "Passives/Inductors", "10\xC2\xB5H", "1210")); // 8
        alpha.parts.push_back(make_part("NOCAT", "No category", ""));                                          // 9

        library::library beta = tests::make_library("beta", "2.0");
        beta.parts.push_back(make_part("LM358", "LM358 (beta)", "ICs/Amplifiers/Op-Amps", "LM358", "DIP-8"));
        beta.parts.push_back(make_part("R_10M", "10M\xCE\xA9", "Passives/Resistors", "10M", "1206"));
        beta.parts.push_back(make_part("R_10m", "10m\xCE\xA9 shunt", "Passives/Resistors", "10m", "2512"));

        library::catalog c;
        c.add(std::move(alpha));
        c.add(std::move(beta));
        return c;
    }

    std::vector<std::string> ids(const library::catalog &c, const library::search_results &r)
    {
        std::vector<std::string> out;
        for (const library::search_hit &hit : r.hits)
            out.push_back(c.qualified_id(hit.ref));
        return out;
    }

    library::search_results search(const library::catalog &c, std::string text, std::string lib = {},
                                   std::string category = {}, std::size_t limit = 0)
    {
        library::search_query q;
        q.text = std::move(text);
        q.library = std::move(lib);
        q.category = std::move(category);
        q.limit = limit;
        return c.search(q);
    }

    void lookup()
    {
        const library::catalog c = sample_catalog();
        CHECK(c.libraries().size() == 2);
        CHECK(c.part_count() == 13);
        CHECK(c.find_library("beta") != nullptr);
        CHECK(c.find_library("gamma") == nullptr);

        const auto a = c.find("alpha:LM358");
        const auto b = c.find("beta:LM358");
        CHECK(a && b && !(*a == *b));
        if (a && b)
        {
            CHECK(c.get(*a).name == "LM358");
            CHECK(c.get(*b).package == "DIP-8");
            CHECK(c.library_of(*b).name == "beta");
            CHECK(c.qualified_id(*a) == "alpha:LM358");
            CHECK(c.find(c.qualified_id(*b)) == b);
        }
        CHECK(c.find("alpha", "R_10k_0603").has_value());
        CHECK(!c.find("alpha:lm358"));
        CHECK(!c.find("gamma:LM358"));
        CHECK(!c.find("LM358"));
        CHECK(!c.find(""));
        CHECK(!c.find("alpha:"));
    }

    void ranking()
    {
        const library::catalog c = sample_catalog();

        // Exact value matches first (both libraries), then a prefix, then a
        // word in a description.
        const auto lm = ids(c, search(c, "lm358"));
        CHECK(lm.size() == 4);
        if (lm.size() == 4)
        {
            CHECK(lm[0] == "alpha:LM358");
            CHECK(lm[1] == "beta:LM358");
            CHECK(lm[2] == "alpha:LM3580");
            CHECK(lm[3] == "alpha:XY1");
        }
        CHECK(ids(c, search(c, "LM358")) == lm);
        CHECK(ids(c, search(c, "  Lm358\t")) == lm);

        // A prefix of the mpn.
        const auto mpn = ids(c, search(c, "rc0603"));
        CHECK(mpn.size() == 1 && mpn[0] == "alpha:R_10k_0603");

        // Every term must match (AND).
        const auto both = ids(c, search(c, "10k 0603"));
        CHECK(both.size() == 1 && both[0] == "alpha:R_10k_0603");
        CHECK(search(c, "10k 0603 nonsense").hits.empty());

        // The exact values first; "10k" is not inside "100k".
        const auto tens = ids(c, search(c, "10k"));
        CHECK(tens.size() == 2);
        CHECK(std::find(tens.begin(), tens.end(), "alpha:R_100k_0603") == tens.end());

        // Inside a word ranks below a word start.
        const auto amp = search(c, "amp");
        CHECK(amp.hits.size() == 2); // "amplifier" in XY1's name, "op-amp" in LM358's description
        if (amp.hits.size() == 2)
        {
            CHECK(c.qualified_id(amp.hits[0].ref) == "alpha:XY1");
            CHECK(amp.hits[0].score > amp.hits[1].score);
        }
        const auto inside = ids(c, search(c, "358"));
        CHECK(inside.size() == 4);

        CHECK(search(c, "zzz").hits.empty());
        CHECK(search(c, "zzz").matched == 0);
    }

    void quantities()
    {
        const library::catalog c = sample_catalog();
        // Written differently, the same quantity.
        CHECK(ids(c, search(c, "4k7")) == std::vector<std::string>{"alpha:R_4k7_0603"});
        CHECK(ids(c, search(c, "4700")) == std::vector<std::string>{"alpha:R_4k7_0603"});
        CHECK(ids(c, search(c, "0.1u")) == std::vector<std::string>{"alpha:C_100n_0603"});
        CHECK(ids(c, search(c, "100n")) == std::vector<std::string>{"alpha:C_100n_0603"});
        CHECK(ids(c, search(c, "0.1uF")) == std::vector<std::string>{"alpha:C_100n_0603"});
        CHECK(search(c, "0.1uH").hits.empty()); // the wrong unit

        // Micro, however it is typed.
        CHECK(ids(c, search(c, "10uH")) == std::vector<std::string>{"alpha:L_10u"});
        CHECK(ids(c, search(c, "10\xC2\xB5H")) == std::vector<std::string>{"alpha:L_10u"});
        CHECK(ids(c, search(c, "10\xCE\xBCH")) == std::vector<std::string>{"alpha:L_10u"});

        // Case matters for prefixes as a number, not as text: "10M" is
        // mega, "10m" milli, and as text both find both.
        const auto mega = ids(c, search(c, "10M", "beta"));
        CHECK(mega.size() == 2 && mega[0] == "beta:R_10M");
        const auto milli = ids(c, search(c, "10m", "beta"));
        CHECK(milli.size() == 2 && milli[0] == "beta:R_10m");
    }

    library::parameter param(std::string name, std::string value, std::string unit)
    {
        library::parameter q;
        q.name = std::move(name);
        q.value = std::move(value);
        q.unit = std::move(unit);
        return q;
    }

    // Parts that say what they are (a device, parameters), as the shipped
    // libraries' do.
    library::part device_part(std::string id, std::string name, std::string device, std::string value,
                              std::string package, std::vector<library::parameter> parameters, std::string mpn = {})
    {
        library::part p = make_part(std::move(id), std::move(name), "Parts", std::move(value), std::move(package));
        p.device = std::move(device);
        p.parameters = std::move(parameters);
        p.mpn = std::move(mpn);
        return p;
    }

    library::catalog device_catalog()
    {
        library::library lib = tests::make_library("dev", "1.0");
        const auto resistor = [&](std::string id, std::string value, std::string package, std::string tolerance,
                                  std::string power, std::string mpn) {
            lib.parts.push_back(device_part(id, value + "\xCE\xA9 " + tolerance + "% " + package, "resistor", value,
                                            package,
                                            {param("Resistance", value, "\xCE\xA9"),
                                             param("Tolerance", tolerance, "%"),
                                             param("Power rating", power, "W"),
                                             param("Voltage rating", "50", "V")},
                                            mpn));
        };
        resistor("R1k_0603_1", "1k", "0603", "1", "100m", "RC0603FR-071KL");   // 0
        resistor("R1k_0603_5", "1k", "0603", "5", "100m", "RC0603JR-071KL");   // 1
        resistor("R1k_0805_1", "1k", "0805", "1", "125m", "RC0805FR-071KL");   // 2
        resistor("R11k_0603", "11k", "0603", "1", "100m", "RC0603FR-0711KL");  // 3
        resistor("R5k1_0603", "5.1k", "0603", "5", "100m", "RC0603JR-075K1L"); // 4
        resistor("R1k1_0603", "1.1k", "0603", "1", "100m", "RC0603FR-071K1L"); // 5
        resistor("R100_0603", "100", "0603", "1", "100m", "RC0603FR-07100RL"); // 6
        lib.parts.push_back(device_part("C100n_0603", "100nF 50V X7R 0603", "capacitor", "100nF", "0603",
                                        {param("Capacitance", "100n", "F"),
                                         param("Voltage rating", "50", "V"),
                                         param("Dielectric", "X7R", ""),
                                         param("Tolerance", "10", "%")})); // 7
        lib.parts.push_back(device_part("C100n_0603_16", "100nF 16V X7R 0603", "capacitor", "100nF", "0603",
                                        {param("Capacitance", "100n", "F"), param("Voltage rating", "16", "V")})); // 8
        lib.parts.push_back(device_part("C1p_0402", "1pF 50V C0G 0402", "capacitor", "1pF", "0402",
                                        {param("Capacitance", "1p", "F"), param("Tolerance", "0.25", "pF")})); // 9
        // A diode: its value is a part number, so a number is text to it.
        lib.parts.push_back(device_part("D1", "1N4148", "diode", "1N4148", "SOD-123",
                                        {param("Reverse voltage", "100", "V"), param("Forward current", "200m", "A")},
                                        "1N4148W")); // 10
        lib.parts.push_back(device_part("U1", "LP5907-3.3 (SOT-23-5)", "", "LP5907-3.3", "SOT-23-5", {})); // 11
        lib.parts.push_back(device_part("Q1", "BC847", "bjt", "BC847", "SOT-23", {})); // 12
        auto by_footprint = device_part("R1k_fp", "1k thin film", "resistor", "1k", "", {});
        by_footprint.footprint = "Resistor_SMD:R_1206_3216Metric";
        lib.parts.push_back(by_footprint); // 13
        resistor("R2k_1206", "2k", "1206", "1", "250m", "RC1206FR-072KL"); // 14
        library::catalog c;
        c.add(std::move(lib));
        return c;
    }

    void values()
    {
        const library::catalog c = device_catalog();
        const auto has = [](const std::vector<std::string> &found, std::string_view id) {
            return std::ranges::find(found, "dev:" + std::string(id)) != found.end();
        };

        // A value is a resistor's resistance, numerically: 11k, 5.1k and
        // 1.1k have "1k" in their text and are not 1 kΩ.
        const auto one_k = ids(c, search(c, "1k"));
        CHECK(one_k.size() == 4);
        CHECK(has(one_k, "R1k_0603_1") && has(one_k, "R1k_0603_5") && has(one_k, "R1k_0805_1") && has(one_k, "R1k_fp"));
        CHECK(!has(one_k, "R11k_0603") && !has(one_k, "R5k1_0603") && !has(one_k, "R1k1_0603"));
        CHECK(ids(c, search(c, "1000")) == one_k);
        CHECK(ids(c, search(c, "1K")) == one_k);
        CHECK(ids(c, search(c, "1k\xCE\xA9")) == one_k);
        CHECK(ids(c, search(c, "1kohm")) == one_k);
        CHECK(search(c, "1kF").hits.empty()); // no capacitor is 1 kF, and a resistor is in Ω
        CHECK(ids(c, search(c, "5k1")) == std::vector<std::string>{"dev:R5k1_0603"});
        CHECK(ids(c, search(c, "100")) == std::vector<std::string>{"dev:R100_0603"});

        // Tolerance, package and power narrow it.
        CHECK(ids(c, search(c, "1k 0603 1%")) == std::vector<std::string>{"dev:R1k_0603_1"});
        CHECK(ids(c, search(c, "1k 0603 \xC2\xB1" "1%")) == std::vector<std::string>{"dev:R1k_0603_1"});
        CHECK(ids(c, search(c, "1k 1/10W")).size() == 2);
        CHECK(ids(c, search(c, "1k 100mW")) == ids(c, search(c, "1k 0.1W")));
        CHECK(ids(c, search(c, "1k 125mW")) == std::vector<std::string>{"dev:R1k_0805_1"});
        // The footprint says the package when the package field does not.
        CHECK(ids(c, search(c, "1k 1206")) == std::vector<std::string>{"dev:R1k_fp"});

        // Capacitors: the same capacitance written three ways; a rating.
        const auto hundred_n = ids(c, search(c, "100n"));
        CHECK(hundred_n.size() == 2);
        CHECK(ids(c, search(c, "0.1u")) == hundred_n);
        CHECK(ids(c, search(c, "100nF")) == hundred_n);
        CHECK(ids(c, search(c, "100nf")) == hundred_n);
        CHECK(ids(c, search(c, "100n 50V")) == std::vector<std::string>{"dev:C100n_0603"});
        CHECK(ids(c, search(c, "100n 50v x7r")) == std::vector<std::string>{"dev:C100n_0603"});
        CHECK(ids(c, search(c, "1p")) == std::vector<std::string>{"dev:C1p_0402"});

        // A rating on its own: every part with it, whatever it is.
        const auto fifty = ids(c, search(c, "50V"));
        CHECK(has(fifty, "R1k_0603_1") && has(fifty, "C100n_0603") && !has(fifty, "C100n_0603_16"));

        // A package code is the package: SOT-23 is not SOT-23-5, though
        // the LP5907's name says "SOT-23".
        CHECK(ids(c, search(c, "sot-23")) == std::vector<std::string>{"dev:Q1"});
        CHECK(ids(c, search(c, "SOT-23-5")) == std::vector<std::string>{"dev:U1"});

        // A part number is text: "4148" is inside a diode's name, and a
        // long number may begin an MPN word of a resistor ("0603" is a
        // package, so it is not that).
        CHECK(ids(c, search(c, "4148")) == std::vector<std::string>{"dev:D1"});
        CHECK(ids(c, search(c, "1n4148")) == std::vector<std::string>{"dev:D1"});
        CHECK(ids(c, search(c, "100V")) == std::vector<std::string>{"dev:D1"});

        // MPNs and plain words as ever.
        CHECK(ids(c, search(c, "RC0603FR-071KL")) == std::vector<std::string>{"dev:R1k_0603_1"});
        CHECK(ids(c, search(c, "rc0603jr")).size() == 2);
        CHECK(ids(c, search(c, "thin film")) == std::vector<std::string>{"dev:R1k_fp"});

        // The principal value outranks a text match of the same term.
        library::library extra = tests::make_library("text", "1.0");
        extra.parts.push_back(make_part("NOTE", "Note 1k pull-up", "Parts"));
        library::catalog both = device_catalog();
        both.add(std::move(extra));
        const auto mixed = ids(both, search(both, "1k"));
        CHECK(mixed.size() == 5 && mixed.back() == "text:NOTE");

        // One part, outside the catalog, scored as search() would.
        const library::part &r = c.get(*c.find("dev:R1k_0603_1"));
        CHECK(c.match(r, "1k 0603 1%") != 0);
        CHECK(c.match(r, "1k 0603 5%") == 0);
        CHECK(c.match(r, "") != 0);
        CHECK(c.match(c.get(*c.find("dev:R11k_0603")), "1k") == 0);
        library::part loose = device_part("X", "Mine", "resistor", "2.2k", "0201", {});
        CHECK(c.match(loose, "2k2 0201") != 0); // its own package counts as one
        CHECK(c.match(loose, "2k2 0402") == 0);
    }

    void quantities_read()
    {
        using library::quantity_unit;
        const auto q = [](std::string_view text) { return library::query_quantity(text); };
        CHECK(q("1k") == library::quantity{1000.0, quantity_unit::none});
        CHECK(q("4k7") && library::same_value(q("4k7")->value, 4700.0));
        CHECK(q("4r7") && library::same_value(q("4r7")->value, 4.7));
        CHECK(q("100nF") && q("100nF")->unit == quantity_unit::farad && library::same_value(q("100nF")->value, 1e-7));
        CHECK(q("10uf") && q("10uf")->unit == quantity_unit::farad);
        CHECK(q("20mA") && q("20mA")->unit == quantity_unit::ampere && library::same_value(q("20mA")->value, 0.02));
        CHECK(q("1/10W") && q("1/10W")->unit == quantity_unit::watt && library::same_value(q("1/10W")->value, 0.1));
        CHECK(q("\xC2\xB1" "5%") == library::quantity{5.0, quantity_unit::percent});
        CHECK(q("2.2kohm") && q("2.2kohm")->unit == quantity_unit::ohm);
        CHECK(!q("3mm") && !q("0710RL") && !q("x7r") && !q("1N4148") && !q("1/0W") && !q(""));

        CHECK(library::parameter_quantity(param("Tolerance", "0.25", "pF")) &&
              library::same_value(library::parameter_quantity(param("Tolerance", "0.25", "pF"))->value, 0.25e-12));
        CHECK(library::parameter_quantity(param("Power rating", "62.5m", "W")) ==
              library::quantity{0.0625, quantity_unit::watt});
        CHECK(!library::parameter_quantity(param("Dielectric", "X7R", "")));
        CHECK(!library::parameter_quantity(param("Temperature coefficient", "100", "ppm/K")));

        library::part r = device_part("R", "R", "Resistor", "", "", {param("ohms", "4.7k", "")});
        CHECK(library::principal_value(r) == library::quantity{4700.0, quantity_unit::ohm});
        r.parameters.clear();
        r.value = "10k";
        CHECK(library::principal_value(r) == library::quantity{10000.0, quantity_unit::ohm});
        r.value = "10µH";
        CHECK(!library::principal_value(r)); // not a resistance
        CHECK(!library::principal_value(device_part("D", "D", "diode", "1N4148", "", {})));
        CHECK(library::principal_of("zener").name == "Zener voltage");
    }

    void filters()
    {
        const library::catalog c = sample_catalog();
        CHECK(ids(c, search(c, "lm358", "beta")) == std::vector<std::string>{"beta:LM358"});
        CHECK(search(c, "lm358", "gamma").hits.empty());

        // A category includes the ones below it, at segment boundaries.
        CHECK(search(c, "lm358", {}, "ICs").hits.size() == 4);
        CHECK(search(c, "lm358", {}, "ICs/Amplifiers/Op-Amps").hits.size() == 1);
        CHECK(search(c, "", {}, "Passives/Res").hits.empty());
        CHECK(search(c, "", {}, " Passives / Resistors ").hits.size() == 6);
        CHECK(search(c, "", "alpha", "Passives/Resistors").hits.size() == 4);

        // An empty query browses: everything, in catalog order.
        const auto all = search(c, "");
        CHECK(all.hits.size() == 13);
        CHECK(all.matched == 13);
        bool ordered = true;
        for (std::size_t i = 1; i < all.hits.size(); ++i)
            ordered = ordered && all.hits[i - 1].ref < all.hits[i].ref;
        CHECK(ordered);

        // A limit keeps the best, and says how many there were.
        const auto limited = search(c, "lm358", {}, {}, 2);
        CHECK(limited.hits.size() == 2 && limited.matched == 4);
        CHECK(ids(c, limited) == (std::vector<std::string>{"alpha:LM358", "beta:LM358"}));
        CHECK(search(c, "", {}, {}, 3).hits.size() == 3);
    }

    void categories()
    {
        const library::catalog c = sample_catalog();
        const library::category_node root = c.categories();
        CHECK(root.parts == 13);
        CHECK(root.own_parts == 1); // NOCAT
        CHECK(root.children.size() == 2);
        if (root.children.size() == 2)
        {
            CHECK(root.children[0].name == "ICs");
            CHECK(root.children[1].name == "Passives");
            CHECK(root.children[1].parts == 8);
            CHECK(root.children[1].path == "Passives");
        }
        const library::category_node *amps = root.find("ICs/Amplifiers");
        CHECK(amps != nullptr);
        if (amps)
        {
            CHECK(amps->parts == 4);
            CHECK(amps->own_parts == 3);
            CHECK(amps->children.size() == 1 && amps->children[0].path == "ICs/Amplifiers/Op-Amps");
        }
        const library::category_node *res = root.find("Passives/Resistors");
        CHECK(res && res->parts == 6 && res->name == "Resistors");
        CHECK(root.find("Passives/Nothing") == nullptr);
        CHECK(root.find("") == &root);

        // Children in name order.
        const library::category_node *passives = root.find("Passives");
        CHECK(passives && passives->children.size() == 3);
        if (passives && passives->children.size() == 3)
        {
            CHECK(passives->children[0].name == "Capacitors");
            CHECK(passives->children[1].name == "Inductors");
            CHECK(passives->children[2].name == "Resistors");
        }

        const library::category_node beta = c.categories("beta");
        CHECK(beta.parts == 3);
        CHECK(beta.find("Passives/Resistors") && beta.find("Passives/Resistors")->parts == 2);
        CHECK(beta.find("Passives/Capacitors") == nullptr);
        CHECK(c.categories("gamma").parts == 0);
    }

    void changes()
    {
        library::catalog c = sample_catalog();
        library::library replacement = tests::make_library("beta", "2.1");
        replacement.parts.push_back(make_part("ONLY", "Only", "Misc"));
        c.add(std::move(replacement));
        CHECK(c.libraries().size() == 2);
        CHECK(c.libraries()[1].version == "2.1");
        CHECK(c.part_count() == 11);
        CHECK(!c.find("beta:LM358"));
        CHECK(c.find("beta:ONLY").has_value());
        CHECK(search(c, "lm358").hits.size() == 3);

        CHECK(c.remove("alpha"));
        CHECK(!c.remove("alpha"));
        CHECK(c.part_count() == 1);
        CHECK(c.find("beta:ONLY") == library::part_ref{0, 0});
        CHECK(c.categories().parts == 1);
        CHECK(search(c, "").hits.size() == 1);

        c.clear();
        CHECK(c.part_count() == 0 && c.libraries().empty());
        CHECK(search(c, "anything").hits.empty());
    }

    void concurrent_reads()
    {
        // Const access from many threads at once, with no locks: the
        // contract the interface relies on.
        const library::catalog c = sample_catalog();
        const auto expected = ids(c, search(c, "lm358"));
        std::atomic<int> mismatches{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t)
        {
            threads.emplace_back([&] {
                for (int i = 0; i < 200; ++i)
                {
                    if (ids(c, search(c, "lm358")) != expected || c.categories().parts != 13 || !c.find("beta:R_10M"))
                        ++mismatches;
                }
            });
        }
        for (std::thread &t : threads)
            t.join();
        CHECK(mismatches == 0);
    }

    std::string library_text(std::string_view name, std::string_view part_id)
    {
        return std::format(R"({{"format": "volt-library", "format_version": 1, "name": "{}", "parts": [{{"id": "{}", "name": "{}"}}]}})",
                           name, part_id, part_id);
    }

    void scanning()
    {
        const tests::scratch_directory directory("volt_catalog_tests \xCE\xA9");
        const std::filesystem::path shipped = directory / "shipped";
        const std::filesystem::path user = directory / "user";
        CHECK(tests::write_text(shipped / "a.voltlib", library_text("a", "A1")));
        CHECK(tests::write_text(shipped / "nested/deeper/b.VOLTLIB", library_text("b", "B1")));
        CHECK(tests::write_text(shipped / "notes.txt", "not a library"));
        CHECK(tests::write_text(shipped / "broken.voltlib", "{ nope"));
        CHECK(tests::write_text(user / "\xD0\xBC\xD0\xBE\xD0\xB8.voltlib", library_text("mine", "M1")));
        CHECK(tests::write_text(user / "a-again.voltlib", library_text("a", "A2"))); // a second "a"

        const std::vector<std::filesystem::path> directories = {shipped, user, directory / "missing"};
        const auto found = library::discover_libraries(directories);
        CHECK(found.size() == 5);

        // The same directory twice, or one inside another, finds each file once.
        const std::vector<std::filesystem::path> overlapping = {shipped, shipped / "nested", shipped};
        CHECK(library::discover_libraries(overlapping).size() == 3);

        library::scan_report report;
        const library::catalog c = library::load_catalog(directories, report);
        CHECK(report.libraries == 3);
        CHECK(report.parts == 3);
        CHECK(report.files.size() == 5);
        CHECK(!report.clean());
        CHECK(c.find("a:A1").has_value()); // the first "a" wins
        CHECK(!c.find("a:A2").has_value());
        CHECK(c.find("b:B1").has_value());
        CHECK(c.find("mine:M1").has_value());
        std::size_t failed = 0;
        for (const auto &f : report.files)
        {
            if (!f.loaded)
            {
                ++failed;
                CHECK(!f.error.empty());
            }
        }
        CHECK(failed == 2);
        const std::string summary = report.summary();
        CHECK(summary.find("already loaded") != std::string::npos);
        CHECK(summary.find("broken.voltlib") != std::string::npos);

        // Progress, and cancelling part way.
        std::size_t calls = 0;
        library::scan_options options;
        options.progress = [&](std::size_t done, std::size_t total, const std::filesystem::path &) {
            CHECK(total == 5);
            CHECK(done == calls);
            ++calls;
            return done < 2;
        };
        library::scan_report partial;
        const library::catalog some = library::load_catalog(directories, partial, options);
        CHECK(partial.cancelled);
        CHECK(partial.files.size() == 2);
        CHECK(calls == 3);
        CHECK(some.libraries().size() <= 2);

        // Nothing to scan is an empty catalog, not an error.
        library::scan_report empty;
        const std::vector<std::filesystem::path> nowhere = {directory / "missing"};
        CHECK(library::load_catalog(nowhere, empty).part_count() == 0);
        CHECK(empty.clean());
    }

    // A library file the user opened wins over one of the same name in the
    // directories, which is shadowed: reported, but not as a failure.
    void shadowing()
    {
        const tests::scratch_directory directory("volt_catalog_shadow_tests");
        const std::filesystem::path shipped = directory / "shipped";
        const std::filesystem::path elsewhere = directory / "downloads";
        CHECK(tests::write_text(shipped / "a.voltlib", library_text("a", "A1")));
        CHECK(tests::write_text(shipped / "b.voltlib", library_text("b", "B1")));
        CHECK(tests::write_text(elsewhere / "a-newer.voltlib", library_text("a", "A2")));
        CHECK(tests::write_text(elsewhere / "c.voltlib", library_text("c", "C1")));
        const std::vector<std::filesystem::path> directories = {shipped};

        library::scan_options options;
        options.files = {elsewhere / "a-newer.voltlib", elsewhere / "c.voltlib"};
        library::scan_report report;
        const library::catalog c = library::load_catalog(directories, report, options);
        CHECK(c.find("a:A2").has_value()); // the opened one
        CHECK(!c.find("a:A1").has_value());
        CHECK(c.find("b:B1").has_value() && c.find("c:C1").has_value());
        CHECK(report.libraries == 3);
        CHECK(report.clean()); // shadowing is no problem
        CHECK(report.files.size() == 4);

        // The opened files come first and say so; the newer "a" names the
        // one it shadows, which says what shadows it.
        CHECK(report.files.size() == 4 && report.files[0].opened && report.files[1].opened && !report.files[2].opened);
        const auto shadowed = std::ranges::find_if(report.files, [](const auto &f) { return f.shadowed; });
        CHECK(shadowed != report.files.end() && !shadowed->loaded && shadowed->path == shipped / "a.voltlib");
        CHECK(shadowed != report.files.end() && shadowed->error.find("a-newer.voltlib") != std::string::npos);
        CHECK(report.files[0].shadows == shipped / "a.voltlib");
        CHECK(report.files[1].shadows.empty());
        CHECK(report.summary().find("shadowed by") != std::string::npos);
        CHECK(report.summary().find("not loaded") == std::string::npos);

        // An opened file that is also in a directory is loaded once.
        library::scan_options again;
        again.files = {shipped / "b.voltlib"};
        library::scan_report once;
        (void)library::load_catalog(directories, once, again);
        CHECK(once.files.size() == 2 && once.libraries == 2 && once.clean());

        // Two opened files of one name: the first given wins, the second is
        // refused as before (not shadowed: the user opened both).
        library::scan_options both;
        both.files = {elsewhere / "a-newer.voltlib", shipped / "a.voltlib"};
        library::scan_report two;
        const library::catalog d = library::load_catalog(std::span<const std::filesystem::path>{}, two, both);
        CHECK(d.find("a:A2").has_value() && !d.find("a:A1").has_value());
        CHECK(two.files.size() == 2 && !two.files[1].loaded && !two.files[1].shadowed && !two.clean());

        // An opened file that cannot be read is a failure like any other.
        library::scan_options missing;
        missing.files = {elsewhere / "gone.voltlib"};
        library::scan_report gone;
        (void)library::load_catalog(directories, gone, missing);
        CHECK(!gone.clean() && gone.files.size() == 3 && gone.files[0].opened && !gone.files[0].loaded);
    }
} // namespace

int main()
{
    lookup();
    ranking();
    quantities();
    values();
    quantities_read();
    filters();
    categories();
    changes();
    concurrent_reads();
    scanning();
    shadowing();
    return volt::tests::result("library catalog");
}
