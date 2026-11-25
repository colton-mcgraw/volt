#include "../math/check.hpp"

#include <app/ui/windows/library_model.hpp>
#include <app/ui/windows/part_search.hpp>

#include <algorithm>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// The library windows' model without a window: the parts list made from a
// catalog, a filter and an order; finding the selection again after the
// catalog is replaced by a rescan; the scope tree's counts; the project
// library's rows and their status against the catalog.

using namespace volt;
using ui::part_column;

namespace
{
    library::part make_part(std::string id, std::string name, std::string value, std::string package,
                            std::string category, std::string manufacturer = "Yageo", std::string mpn = {})
    {
        library::part p;
        p.id = std::move(id);
        p.name = std::move(name);
        p.value = std::move(value);
        p.package = std::move(package);
        p.category = std::move(category);
        p.manufacturer = std::move(manufacturer);
        p.mpn = std::move(mpn);
        return p;
    }

    library::library resistors()
    {
        library::library lib;
        lib.name = "res";
        lib.title = "Resistors";
        lib.version = "1";
        lib.parts.push_back(make_part("R_10k", "10k 0603", "10k", "0603", "Passives/Resistors", "Yageo", "RC10K"));
        lib.parts.push_back(make_part("R_4k7", "4.7k 0603", "4.7k", "0603", "Passives/Resistors", "Yageo", "RC4K7"));
        lib.parts.push_back(make_part("R_100", "100R 0402", "100", "0402", "Passives/Resistors", "Vishay", "CRCW100"));
        lib.parts.push_back(make_part("R_2M", "2M 0805", "2M", "0805", "Passives/Resistors", "", ""));
        return lib;
    }

    library::library chips()
    {
        library::library lib;
        lib.name = "ics";
        lib.title = "ICs";
        lib.version = "1";
        lib.parts.push_back(make_part("LM358", "LM358 dual op amp", "LM358", "SOIC-8", "ICs/Amplifiers", "TI", "LM358DR"));
        lib.parts.push_back(make_part("NE555", "NE555 timer", "NE555", "SOIC-8", "ICs/Timers", "TI", "NE555DR"));
        return lib;
    }

    std::shared_ptr<const library::catalog> make_catalog(bool chips_first = false)
    {
        auto c = std::make_shared<library::catalog>();
        if (chips_first)
            c->add(chips());
        c->add(resistors());
        if (!chips_first)
            c->add(chips());
        return c;
    }

    ui::part_filter filter(std::string text, std::string library, std::string category)
    {
        return {std::move(text), std::move(library), std::move(category)};
    }

    std::vector<std::string> names(const ui::part_list &list)
    {
        std::vector<std::string> out;
        for (std::size_t i = 0; i < list.size(); ++i)
            out.emplace_back(list.cell(i, part_column::name));
        return out;
    }

    void natural_order()
    {
        CHECK(ui::natural_compare("R2", "R10") < 0);
        CHECK(ui::natural_compare("R10", "R2") > 0);
        CHECK(ui::natural_compare("abc", "ABC") == 0);
        CHECK(ui::natural_compare("C007", "C7") == 0);
        CHECK(ui::natural_compare("0402", "0603") < 0);
        CHECK(ui::natural_compare("", "a") < 0);
        CHECK(ui::natural_compare("a1b", "a1c") < 0);
    }

    void search_and_count()
    {
        ui::part_list list;
        CHECK(list.size() == 0);
        CHECK(list.total() == 0);

        list.set_catalog(make_catalog());
        CHECK(list.total() == 6);
        CHECK(list.size() == 6); // no filter: every part, catalog order
        CHECK(list.cell(0, part_column::name) == "10k 0603");
        CHECK(list.cell(4, part_column::library) == "ics");
        CHECK(list.qualified_id(0) == "res:R_10k");

        CHECK(list.set_filter(filter("0603", "", "")));
        CHECK(list.size() == 2);
        CHECK(!list.set_filter(filter("0603", "", ""))); // the same: nothing runs

        // Every term must match.
        list.set_filter(filter("0603 4.7k", "", ""));
        CHECK(list.size() == 1);
        CHECK(list.qualified_id(0) == "res:R_4k7");

        // A library, a category.
        list.set_filter(filter("", "ics", ""));
        CHECK(list.size() == 2);
        list.set_filter(filter("", "", "ICs/Timers"));
        CHECK(list.size() == 1);
        CHECK(list.cell(0, part_column::mpn) == "NE555DR");
        list.set_filter(filter("lm358", "res", ""));
        CHECK(list.size() == 0);
    }

    void sorting()
    {
        ui::part_list list;
        list.set_catalog(make_catalog());
        list.set_filter(filter("", "res", ""));

        // By value as a quantity: 100 < 4.7k < 10k < 2M, not as text.
        list.set_sort(part_column::value, true);
        CHECK(names(list) == std::vector<std::string>{"100R 0402", "4.7k 0603", "10k 0603", "2M 0805"});
        list.set_sort(part_column::value, false);
        CHECK(names(list).front() == "2M 0805");

        // By package, naturally; ties keep the search's order.
        list.set_sort(part_column::package, true);
        CHECK(names(list) == std::vector<std::string>{"100R 0402", "10k 0603", "4.7k 0603", "2M 0805"});

        // Empty cells go last either way.
        list.set_sort(part_column::manufacturer, true);
        CHECK(names(list).back() == "2M 0805");
        list.set_sort(part_column::manufacturer, false);
        CHECK(names(list).back() == "2M 0805");
        CHECK(list.cell(0, part_column::manufacturer) == "Yageo");

        // Back to the search's order.
        list.set_sort(std::nullopt, true);
        CHECK(names(list).front() == "10k 0603");
        CHECK(!list.sort_column());
    }

    void selection_survives_a_rescan()
    {
        ui::part_list list;
        list.set_catalog(make_catalog());
        list.set_filter(filter("soic", "", ""));
        const auto before = list.find("ics:NE555");
        CHECK(before.has_value());

        // A rescan: a new catalog, the libraries in another order, so the
        // refs differ; the qualified id finds the part again.
        auto rescanned = make_catalog(true);
        list.set_catalog(rescanned);
        CHECK(list.catalog() == rescanned.get());
        CHECK(list.filter().text == "soic"); // the filter stays
        const auto after = list.find("ics:NE555");
        CHECK(after.has_value());
        if (after)
            CHECK(list.qualified_id(*after) == "ics:NE555");

        // Gone from the catalog, or filtered out: not found.
        CHECK(!list.find("res:R_10k").has_value());
        CHECK(!list.find("nope:R_10k").has_value());
        CHECK(!list.find("").has_value());

        list.set_catalog(nullptr);
        CHECK(list.size() == 0);
        CHECK(!list.find("ics:NE555").has_value());
    }

    void scopes()
    {
        const auto c = make_catalog();
        const std::vector<ui::scope_node> tree = ui::scope_tree(*c);
        CHECK(tree.size() == 3); // all, res, ics
        CHECK(tree[0].label == "All libraries");
        CHECK(tree[0].parts == 6);
        CHECK(tree[0].library.empty());
        CHECK(tree[1].label == "Resistors");
        CHECK(tree[1].library == "res");
        CHECK(tree[1].parts == 4);
        CHECK(tree[2].parts == 2);

        // All libraries: ICs (Amplifiers, Timers), Passives (Resistors).
        CHECK(tree[0].children.size() == 2);
        CHECK(tree[0].children[0].label == "ICs");
        CHECK(tree[0].children[0].parts == 2);
        CHECK(tree[0].children[0].children.size() == 2);
        CHECK(tree[0].children[0].children[1].category == "ICs/Timers");
        CHECK(tree[0].children[0].children[1].parts == 1);
        // A library's own categories carry its name.
        CHECK(tree[2].children[0].library == "ics");
    }

    void project_status()
    {
        auto c = make_catalog();
        library::project p = library::new_project("Blinky");
        CHECK(ui::project_rows(p, c.get()).empty());

        CHECK(library::add_part(p, *c, *c->find("res:R_10k")).has_value());
        CHECK(library::add_part(p, *c, *c->find("ics:LM358")).has_value());
        CHECK(library::add_part(p, *c, *c->find("res:R_4k7")).has_value());

        // By name, naturally; all up to date.
        std::vector<ui::project_row> rows = ui::project_rows(p, c.get());
        CHECK(rows.size() == 3);
        CHECK(rows[0].name == "4.7k 0603"); // 4 before 10
        CHECK(rows[1].name == "10k 0603");
        CHECK(rows[2].name == "LM358 dual op amp");
        CHECK(rows[1].source == "res:R_10k");
        for (const ui::project_row &r : rows)
            CHECK(r.status == library::part_status::up_to_date);
        CHECK(rows[0].status_text == "Up to date");

        // While the catalog loads there is no status yet.
        rows = ui::project_rows(p, nullptr);
        CHECK(!rows[0].status.has_value());
        CHECK(rows[0].status_text == ui::status_text(std::nullopt));

        // Edited in the project; changed at the source; both; gone.
        p.parts.find("R_10k")->description = "edited";
        auto changed = std::make_shared<library::catalog>();
        library::library res = resistors();
        res.parts[1].description = "new datasheet"; // R_4k7
        res.parts[0].description = "new too";       // R_10k
        changed->add(res);
        rows = ui::project_rows(p, changed.get());
        const auto row = [&](std::string_view id) {
            return *std::ranges::find_if(rows, [&](const ui::project_row &r) { return r.id == id; });
        };
        CHECK(row("R_10k").status == library::part_status::diverged);
        CHECK(row("R_4k7").status == library::part_status::source_changed);
        CHECK(row("LM358").status == library::part_status::source_missing);
        CHECK(ui::update_loses_edits(library::part_status::diverged));
        CHECK(ui::update_loses_edits(library::part_status::locally_modified));
        CHECK(!ui::update_loses_edits(library::part_status::source_changed));

        // A part made in the project has no source.
        library::part own = make_part("MY1", "My part", "1", "0805", "Misc");
        p.parts.parts.push_back(own);
        rows = ui::project_rows(p, nullptr);
        CHECK(row("MY1").status == library::part_status::unlinked);
        CHECK(row("MY1").source.empty());

        // The project scope's search.
        const ui::project_row r10k = row("R_10k");
        CHECK(ui::matches(r10k, p.parts.find("R_10k"), "10k"));
        CHECK(ui::matches(r10k, p.parts.find("R_10k"), "0603 yageo"));
        CHECK(ui::matches(r10k, p.parts.find("R_10k"), "RC10K"));
        CHECK(!ui::matches(r10k, p.parts.find("R_10k"), "0603 vishay"));
        CHECK(ui::matches(r10k, nullptr, ""));
    }

    void details()
    {
        library::part p = make_part("R_10k", "10k", "10k", "0603", "Passives", "Yageo", "RC10K");
        p.model = "parts/R_0603.step";
        const auto fields = ui::part_fields(p);
        CHECK(!fields.empty());
        CHECK(fields[0].first == "Value");
        CHECK(std::ranges::any_of(fields, [](const auto &f) { return f.first == "MPN" && f.second == "RC10K"; }));
        // Empty fields are left out.
        CHECK(std::ranges::none_of(fields, [](const auto &f) { return f.first == "Footprint"; }));

        const std::filesystem::path models = "assets/models";
        CHECK(ui::model_path(p, models) == models / "parts" / "R_0603.step");
        p.model.clear();
        CHECK(ui::model_path(p, models).empty());

        CHECK(ui::column_title(part_column::mpn) == "MPN");
    }

    library::parameter param(std::string name, std::string value, std::string unit = {})
    {
        library::parameter q;
        q.name = std::move(name);
        q.value = std::move(value);
        q.unit = std::move(unit);
        return q;
    }

    library::part device_part(std::string device, std::string value, std::vector<library::parameter> parameters)
    {
        library::part p = make_part("P", "Part", value, "0603", "Parts");
        p.device = std::move(device);
        p.parameters = std::move(parameters);
        return p;
    }

    // What a row says about a part, per device, in its fixed order.
    void specs()
    {
        const std::string pm = "\xC2\xB1";
        const std::string dot = " \xC2\xB7 ";
        CHECK(ui::spec_summary(device_part("resistor", "1k",
                                           {param("Resistance", "1k", "\xCE\xA9"), param("Power rating", "0.1", "W"),
                                            param("Tolerance", "1", "%"), param("Voltage rating", "75", "V"),
                                            param("Temperature coefficient", "100", "ppm/K"), param("Series", "E96")})) ==
              "1k\xCE\xA9 " + pm + "1% 100mW 100ppm/K");
        // A resistor that only has its value.
        CHECK(ui::spec_summary(device_part("resistor", "4k7", {})) == "4k7");
        CHECK(ui::spec_summary(device_part("capacitor", "100nF",
                                           {param("Capacitance", "100n", "F"), param("Tolerance", "10", "%"),
                                            param("Voltage rating", "50", "V"), param("Dielectric", "X7R")})) ==
              "100nF 50V X7R " + pm + "10%");
        // A tolerance in farads, as written.
        CHECK(ui::spec_summary(device_part("capacitor", "1pF",
                                           {param("Capacitance", "1p", "F"), param("Tolerance", "0.25", "pF")})) ==
              "1pF " + pm + "0.25pF");
        CHECK(ui::spec_summary(device_part("inductor", "10uH",
                                           {param("Inductance", "10u", "H"), param("Current rating", "1.2", "A"),
                                            param("DC resistance", "50m", "\xCE\xA9")})) ==
              "10\xC2\xB5H" + dot + "1.2A" + dot + "DCR 50m\xCE\xA9");
        CHECK(ui::spec_summary(device_part("zener", "5.1V",
                                           {param("Zener voltage", "5.1", "V"), param("Tolerance", "5", "%"),
                                            param("Power rating", "250m", "W")})) ==
              "Zener 5.1V" + dot + pm + "5%" + dot + "250mW");
        CHECK(ui::spec_summary(device_part("led", "Red",
                                           {param("Color", "Red"), param("Wavelength", "630", "nm"),
                                            param("Forward voltage", "2", "V"), param("Forward current", "20m", "A")})) ==
              "Red LED" + dot + "Vf 2V" + dot + "If 20mA");
        CHECK(ui::spec_summary(device_part("diode", "1N4148",
                                           {param("Reverse voltage", "100", "V"), param("Forward current", "200m", "A")})) ==
              "1N4148" + dot + "If 200mA" + dot + "Vr 100V");
        CHECK(ui::spec_summary(device_part("bjt", "BC547B",
                                           {param("Type", "NPN"), param("Collector-emitter voltage", "45", "V"),
                                            param("Collector current", "100m", "A")})) ==
              "BC547B" + dot + "NPN" + dot + "Vce 45V" + dot + "Ic 100mA");
        CHECK(ui::spec_summary(device_part("mosfet", "2N7002",
                                           {param("Type", "N-channel"), param("Drain-source voltage", "60", "V"),
                                            param("Drain current", "115m", "A")})) ==
              "2N7002" + dot + "N-channel" + dot + "Vds 60V" + dot + "Id 115mA");
        // Anything else: the value and the first three parameters.
        CHECK(ui::spec_summary(device_part("", "1x4",
                                           {param("Positions", "4"), param("Rows", "1"), param("Pitch", "2.54", "mm"),
                                            param("Gender", "Male")})) ==
              "1x4" + dot + "Positions 4" + dot + "Rows 1" + dot + "Pitch 2.54mm");
        CHECK(ui::spec_summary(device_part("", "LM358", {})) == "LM358");
        CHECK(ui::spec_summary(device_part("", "", {})).empty());

        library::part made = device_part("resistor", "1k", {param("Resistance", "1k", "\xCE\xA9"), param("Series", "E96")});
        CHECK(ui::parameter_summary(made) == "Resistance 1k\xCE\xA9" + dot + "Series E96");
        made.manufacturer = "Yageo";
        CHECK(ui::maker_text(made) == "Yageo");
        made.mpn = "RC0603FR-071KL";
        CHECK(ui::maker_text(made) == "Yageo RC0603FR-071KL");
        made.manufacturer.clear();
        CHECK(ui::maker_text(made) == "RC0603FR-071KL");

        // A list's rows have them, made as they are asked for.
        auto c = std::make_shared<library::catalog>();
        library::library lib;
        lib.name = "dev";
        lib.version = "1";
        library::part r = device_part("resistor", "1k", {param("Resistance", "1k", "\xCE\xA9"), param("Tolerance", "1", "%")});
        r.id = "R1";
        lib.parts.push_back(r);
        c->add(std::move(lib));
        ui::part_list list;
        list.set_catalog(c);
        CHECK(list.size() == 1 && list.specs(0) == "1k\xCE\xA9 " + pm + "1%");
    }

    // The project scope searches as the catalog does, values and all.
    void project_search()
    {
        library::project p;
        p.parts.name = "project";
        library::part a = device_part("resistor", "1k", {param("Resistance", "1k", "\xCE\xA9")});
        a.id = "R_1k";
        a.name = "1k 0603";
        library::part b = device_part("resistor", "11k", {param("Resistance", "11k", "\xCE\xA9")});
        b.id = "R_11k";
        b.name = "11k 0603";
        p.parts.parts = {b, a};
        const std::vector<ui::project_row> rows = ui::project_rows(p, nullptr);
        const auto catalog = make_catalog();

        const auto shown = ui::project_matches(p, rows, catalog.get(), "1k");
        CHECK(shown.size() == 1 && rows[shown[0]].id == "R_1k");
        CHECK(ui::project_matches(p, rows, catalog.get(), "").size() == 2);
        CHECK(ui::project_matches(p, rows, catalog.get(), "0603").size() == 2);
        // Without a catalog, as text: "1k" is in both.
        CHECK(ui::project_matches(p, rows, nullptr, "1k").size() == 2);
    }
} // namespace

// Long values break at sensible characters, as zero width spaces.
void break_points()
{
    const std::string zw = "\xE2\x80\x8B";
    CHECK(ui::with_break_points("Package_SO:SOIC-8") == "Package_" + zw + "SO:" + zw + "SOIC-8");
    CHECK(ui::with_break_points("parts/R_0603.step") == "parts/" + zw + "R_" + zw + "0603.step");
    // Not at the end, nor before a space; plain text as it is.
    CHECK(ui::with_break_points("Note: see/") == "Note: see/");
    CHECK(ui::with_break_points("10k 1%") == "10k 1%");
    CHECK(ui::with_break_points("").empty());
}

// The background search both library views use (<app/ui/windows/
// part_search.hpp>): what comes back, and that only the newest does.
void background_search()
{
    using volt::ui::part_search;
    std::mutex mutex;
    std::vector<std::function<void()>> posted;
    const auto run_posted = [&] {
        std::vector<std::function<void()>> tasks;
        {
            std::lock_guard lock(mutex);
            tasks.swap(posted);
        }
        for (auto &task : tasks)
            task();
        return tasks.size();
    };
    const auto wait_for = [&](std::size_t count) {
        for (int i = 0; i < 500; ++i)
        {
            {
                std::lock_guard lock(mutex);
                if (posted.size() >= count)
                    return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    };

    const auto catalog = make_catalog();
    volt::ui::part_list list;
    list.set_catalog(catalog);
    CHECK(!part_search::wanted(list)); // a few parts: searched where they are

    std::vector<std::string> done;
    {
        part_search search([&](std::function<void()> task) {
            std::lock_guard lock(mutex);
            posted.push_back(std::move(task));
        });

        // One search: its hits come back, through the post.
        volt::ui::part_filter f;
        f.text = "10k";
        search.start(catalog, f, [&](const auto &from, volt::ui::part_filter filter, auto ranked, double) {
            CHECK(from == catalog);
            CHECK(list.set_results(from, filter, std::move(ranked), 0.0));
            done.push_back(filter.text);
        });
        CHECK(wait_for(1));
        CHECK(run_posted() == 1);
        CHECK(done.size() == 1 && done[0] == "10k");
        CHECK(list.size() > 0 && list.filter().text == "10k");

        // Two in a row: only the newer one reports, whichever order the
        // thread got to them in.
        f.text = "R";
        search.start(catalog, f, [&](const auto &, volt::ui::part_filter filter, auto, double) {
            done.push_back(filter.text);
        });
        f.text = "C";
        search.start(catalog, f, [&](const auto &, volt::ui::part_filter filter, auto, double) {
            done.push_back(filter.text);
        });
        CHECK(wait_for(1));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        run_posted();
        CHECK(done.size() == 2 && done.back() == "C");

        // Cancelled: nothing.
        f.text = "LED";
        search.start(catalog, f, [&](const auto &, volt::ui::part_filter filter, auto, double) {
            done.push_back(filter.text);
        });
        search.cancel();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        run_posted();
        CHECK(done.size() == 2);

        // Started, and the search gone before its result is run.
        f.text = "10k";
        search.start(catalog, f, [&](const auto &, volt::ui::part_filter filter, auto, double) {
            done.push_back(filter.text);
        });
        CHECK(wait_for(1));
    }
    run_posted();
    CHECK(done.size() == 2);
}

int main()
{
    natural_order();
    search_and_count();
    sorting();
    selection_survives_a_rescan();
    scopes();
    project_status();
    details();
    specs();
    project_search();
    background_search();
    break_points();
    return volt::tests::result("library model");
}
