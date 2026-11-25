// The libraries shipped in assets/libraries: every one loads clean, the
// parts are complete, the models they name exist, and the files are what
// the generator (volt/src/library/tools/seed.cpp) writes today, so nobody
// edits them by hand and loses the edit at the next regeneration.

#include "library_test_support.hpp"

#include "seed.hpp"

#include <library/schema.hpp>

#include <set>

using namespace volt;

namespace
{
    const std::filesystem::path libraries_directory = library::path_from_utf8(VOLT_LIBRARY_ASSETS);
    const std::filesystem::path models_directory = library::path_from_utf8(VOLT_MODEL_ASSETS);

    library::catalog load_seed(library::scan_report &report)
    {
        const std::vector<std::filesystem::path> directories = {libraries_directory};
        return library::load_catalog(directories, report);
    }

    void loads_clean()
    {
        library::scan_report report;
        const library::catalog c = load_seed(report);
        if (!report.clean())
            std::fprintf(stderr, "%s\n", report.summary().c_str());
        CHECK(report.clean());
        CHECK(report.libraries >= 6);
        CHECK(c.part_count() >= 3000);
        std::printf("seed: %s\n", report.summary().c_str());

        for (const library::library &lib : c.libraries())
        {
            CHECK(!lib.title.empty());
            CHECK(!lib.version.empty());
            CHECK(!lib.license.empty());
        }

        std::set<std::string> missing_models;
        std::size_t with_model = 0;
        for (const library::library &lib : c.libraries())
        {
            for (const library::part &p : lib.parts)
            {
                const bool complete = !p.name.empty() && !p.category.empty() && !p.value.empty() &&
                                      !p.package.empty() && !p.prefix.empty() && !p.symbol.empty() &&
                                      !p.footprint.empty() && !p.description.empty();
                if (!complete)
                    std::fprintf(stderr, "  incomplete: %s:%s\n", lib.name.c_str(), p.id.c_str());
                CHECK(complete);
                CHECK(p.footprint.find(':') != std::string::npos);

                if (!p.model.empty())
                {
                    ++with_model;
                    if (!std::filesystem::exists(models_directory / library::path_from_utf8(p.model)))
                        missing_models.insert(p.model);
                }

                // Nothing a simulation would stumble on: every parameter is
                // one the schema knows, and every device has what it needs.
                for (const library::issue &found : library::schema::builtin().check(p))
                {
                    std::fprintf(stderr, "  %s:%s %s\n", lib.name.c_str(), p.id.c_str(), found.message.c_str());
                    CHECK(false);
                }

                // Electrical quantities read back as numbers.
                for (const char *quantity : {"Resistance", "Capacitance", "Inductance", "Voltage rating", "Power rating"})
                {
                    if (const library::parameter *q = p.find_parameter(quantity))
                    {
                        if (!q->number())
                            std::fprintf(stderr, "  %s:%s %s = '%s'\n", lib.name.c_str(), p.id.c_str(), quantity,
                                         q->value.c_str());
                        CHECK(q->number().has_value());
                    }
                }
            }
        }
        for (const std::string &model : missing_models)
            std::fprintf(stderr, "  missing model %s\n", model.c_str());
        CHECK(missing_models.empty());
        CHECK(with_model > 100);
    }

    void generator_agrees()
    {
        for (const library::library &lib : library::seed::libraries())
        {
            const std::filesystem::path path = libraries_directory / library::path_from_utf8(lib.name + ".voltlib");
            const auto committed = json::read_text_file(path);
            CHECK(committed.has_value());
            if (!committed)
                continue;
            const bool same = *committed == library::write_library(lib);
            if (!same)
                std::fprintf(stderr, "  %s differs from the generator's output; regenerate it with the "
                                     "volt_generate_libraries target\n",
                             lib.name.c_str());
            CHECK(same);
        }
    }

    void searches()
    {
        library::scan_report report;
        const library::catalog c = load_seed(report);
        const auto first = [&](std::string text, std::string category = {}) -> const library::part * {
            library::search_query q;
            q.text = std::move(text);
            q.category = std::move(category);
            q.limit = 5;
            const auto results = c.search(q);
            return results.hits.empty() ? nullptr : &c.get(results.hits[0].ref);
        };

        const library::part *r = first("10k 0603");
        CHECK(r && r->value == "10k" && r->package == "0603");
        const library::part *opamp = first("lm358");
        CHECK(opamp && opamp->value == "LM358");
        const library::part *cap = first("100n 0402 x7r");
        CHECK(cap && cap->value == "100nF" && cap->package == "0402");
        const library::part *micro = first("4.7uF 0805");
        CHECK(micro && micro->value == "4.7\xC2\xB5" "F");
        const library::part *led = first("led red 0805");
        CHECK(led && led->value == "Red" && led->package == "0805");
        const library::part *usb = first("usb c");
        CHECK(usb && usb->mpn == "USB4105-GF-A");
        const library::part *zener = first("5v1", "Discretes/Diodes/Zener");
        CHECK(zener && zener->value == "5.1V");
        const library::part *header = first("1x04 header");
        CHECK(header && header->value == "1x4");
    }

    // Values mean values over the shipped parts: "1k" is the 1 kΩ
    // resistors, not 11k, 5.1k or 1.1k, which have "1k" in their text.
    void value_searches()
    {
        library::scan_report report;
        const library::catalog c = load_seed(report);
        const auto all = [&](std::string text) {
            library::search_query q;
            q.text = std::move(text);
            return c.search(q).hits;
        };

        const auto one_k = all("1k");
        std::size_t resistors = 0;
        bool leading = true;
        for (const library::search_hit &hit : one_k)
        {
            const library::part &p = c.get(hit.ref);
            const auto value = library::principal_value(p);
            // Every part with a value of its own here is 1 kΩ.
            if (value)
                CHECK(library::same_value(value->value, 1000.0) && value->unit == library::quantity_unit::ohm);
            if (p.device == "resistor")
            {
                ++resistors;
                CHECK(leading); // all of them before anything matched as text
            }
            else
                leading = false;
            CHECK(p.value != "11k" && p.value != "5.1k" && p.value != "1.1k");
        }
        CHECK(resistors == 8); // E96 and E24, four packages each

        // Narrowed to a handful: one, the 1% 0603.
        const auto narrow = all("1k 0603 1%");
        CHECK(!narrow.empty() && narrow.size() <= 3);
        CHECK(!narrow.empty() && c.get(narrow[0].ref).mpn == "RC0603FR-071KL");
        CHECK(all("1k 0603 1/10W").size() == 2);

        // One capacitance, three ways of writing it.
        const auto ids = [&](const std::vector<library::search_hit> &hits) {
            std::vector<std::string> out;
            for (const library::search_hit &hit : hits)
                out.push_back(c.qualified_id(hit.ref));
            return out;
        };
        CHECK(!all("100n").empty());
        CHECK(ids(all("100n")) == ids(all("0.1u")));
        CHECK(ids(all("100nF")) == ids(all("0.1uF")));
        const auto rated = all("100nF 0402 50V");
        CHECK(rated.size() == 1 && c.get(rated[0].ref).mpn == "CC0402KRX7R9BB104");

        // Packages: SOT-23 is not SOT-23-5.
        for (const library::search_hit &hit : all("sot-23"))
            CHECK(c.get(hit.ref).package == "SOT-23");

        // Text and MPNs as before.
        const auto mpn = all("RC0603FR-071KL");
        CHECK(mpn.size() == 1 && c.get(mpn[0].ref).value == "1k");
        const auto lm = all("lm358");
        CHECK(!lm.empty() && c.get(lm[0].ref).value == "LM358");
        const auto diode = all("1n4148");
        CHECK(!diode.empty() && c.get(diode[0].ref).value == "1N4148");
        const auto zener = all("5.1V");
        CHECK(!zener.empty() && c.get(zener[0].ref).device == "zener");
        const auto led = all("20mA red");
        CHECK(!led.empty() && c.get(led[0].ref).device == "led");
    }
} // namespace

int main()
{
    loads_clean();
    generator_agrees();
    searches();
    value_searches();
    return volt::tests::result("seed libraries");
}
