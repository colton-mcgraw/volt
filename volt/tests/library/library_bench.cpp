// Timings of the catalog at the size a real installation reaches: the seed
// libraries copied under other names until there are about 50 000 parts.
// Search has to keep up with typing, so what matters is the time of one
// query, the worst one included, for the queries a person types on the way
// to a part.
//
//   volt_library_bench [copies]

#include <library/catalog.hpp>
#include <library/project.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace volt;

namespace
{
    double milliseconds_since(std::chrono::steady_clock::time_point start)
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
} // namespace

int main(int argc, char **argv)
{
    const int copies = argc > 1 ? std::max(1, std::atoi(argv[1])) : 13;

    auto start = std::chrono::steady_clock::now();
    library::scan_report report;
    const std::vector<std::filesystem::path> directories = {library::path_from_utf8(VOLT_LIBRARY_ASSETS)};
    const library::catalog seed = library::load_catalog(directories, report);
    std::printf("load the seed libraries:   %8.2f ms  (%zu libraries, %zu parts)\n", milliseconds_since(start),
                report.libraries, seed.part_count());
    if (seed.part_count() == 0)
    {
        std::printf("no seed libraries found in %s\n", VOLT_LIBRARY_ASSETS);
        return 1;
    }

    start = std::chrono::steady_clock::now();
    library::catalog big;
    for (int copy = 0; copy < copies; ++copy)
    {
        for (const library::library &lib : seed.libraries())
        {
            library::library renamed = lib;
            renamed.name += "-" + std::to_string(copy);
            big.add(std::move(renamed));
        }
    }
    std::printf("index %2d copies:           %8.2f ms  (%zu parts)\n", copies, milliseconds_since(start),
                big.part_count());

    start = std::chrono::steady_clock::now();
    const library::category_node tree = big.categories();
    std::printf("category tree:             %8.2f ms  (%zu parts)\n", milliseconds_since(start), tree.parts);

    // Prefixes of what people type, one keystroke at a time, and some
    // whole queries.
    const char *queries[] = {
        "1",          "10",         "10k",          "10k ",          "10k 0",       "10k 06",     "10k 0603",
        "l",          "lm",         "lm3",          "lm358",         "c",           "ca",         "cap",
        "capacitor",  "100n",       "100n x7r",     "100n x7r 0402", "4k7",         "0.1u",       "usb",
        "usb c",      "stm32",      "resistor 1%",  "led red",       "zzzz",        "yageo",      "thick film",
    };
    std::printf("\n%-16s %9s %9s %9s\n", "query", "matches", "mean ms", "worst ms");
    double worst_overall = 0.0;
    double total = 0.0;
    std::size_t count = 0;
    for (const char *text : queries)
    {
        library::search_query query;
        query.text = text;
        constexpr int runs = 20;
        double sum = 0.0;
        double worst = 0.0;
        std::size_t matches = 0;
        for (int i = 0; i < runs; ++i)
        {
            const auto t = std::chrono::steady_clock::now();
            const library::search_results results = big.search(query);
            const double ms = milliseconds_since(t);
            matches = results.matched;
            sum += ms;
            worst = std::max(worst, ms);
        }
        std::printf("%-16s %9zu %9.3f %9.3f\n", (std::string("\"") + text + "\"").c_str(), matches, sum / runs, worst);
        worst_overall = std::max(worst_overall, worst);
        total += sum / runs;
        ++count;
    }

    // With a category, and with a limit, as the browser's panel uses it.
    for (const char *category : {"Passives/Resistors", "ICs"})
    {
        library::search_query query;
        query.text = "10";
        query.category = category;
        query.limit = 200;
        const auto t = std::chrono::steady_clock::now();
        const library::search_results results = big.search(query);
        std::printf("\"10\" in %-18s %7zu %9.3f ms (limit 200)\n", category, results.matched, milliseconds_since(t));
    }
    std::printf("\nmean over the queries %.3f ms, worst single query %.3f ms\n", total / static_cast<double>(count),
                worst_overall);
    return 0;
}
