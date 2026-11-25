#include "../math/check.hpp"

#include <app/simulation/network.hpp>

#include <cmath>
#include <utility>
#include <vector>

using namespace volt;
using namespace volt::simulation;

namespace
{
    bool near(float a, float b, float tolerance = 1e-4f)
    {
        return std::abs(a - b) <= tolerance;
    }

    // Sweeps until the potentials stop moving, as the GPU does step by step.
    std::vector<float> solve(const packed_network &packed, int sweeps)
    {
        std::vector<float> from(packed.nodes.size(), 0.0f);
        std::vector<float> to(packed.nodes.size(), 0.0f);
        for (int i = 0; i < sweeps; ++i)
        {
            relax(packed, from, to);
            std::swap(from, to);
        }
        return from;
    }

    void packing()
    {
        network network;
        network.nodes = 3;
        network.branches = {{0, 1, 2.0f}, {1, 2, 4.0f}, {1, 1, 8.0f}, {1, 7, 8.0f}}; // the last two are no branches
        network.holds = {{0, 5.0f}, {9, 1.0f}};
        network.sources = {{1, 0.25f}, {1, 0.25f}, {0, 3.0f}};

        const packed_network packed = pack(network);
        CHECK(packed.nodes.size() == 3);
        CHECK(packed.edges.size() == 4);

        // Every node's edges are one run, and the runs follow each other.
        CHECK(packed.nodes[0].first == 0 && packed.nodes[0].count == 1);
        CHECK(packed.nodes[1].first == 1 && packed.nodes[1].count == 2);
        CHECK(packed.nodes[2].first == 3 && packed.nodes[2].count == 1);

        CHECK(packed.edges[0].other == 1 && near(packed.edges[0].conductance, 2.0f));
        CHECK(packed.edges[1].other == 0 && near(packed.edges[1].conductance, 2.0f));
        CHECK(packed.edges[2].other == 2 && near(packed.edges[2].conductance, 4.0f));
        CHECK(packed.edges[3].other == 1 && near(packed.edges[3].conductance, 4.0f));

        // Held wins over fed; currents into one node add up.
        CHECK(packed.nodes[0].held == 1 && near(packed.nodes[0].value, 5.0f));
        CHECK(packed.nodes[1].held == 0 && near(packed.nodes[1].value, 0.5f));
        CHECK(packed.nodes[2].held == 0 && near(packed.nodes[2].value, 0.0f));
    }

    // 10 V across 100 ohm and 300 ohm in series.
    void divider()
    {
        network network;
        network.nodes = 3;
        network.branches = {{0, 1, 1.0f / 100.0f}, {1, 2, 1.0f / 300.0f}};
        network.holds = {{0, 10.0f}, {2, 0.0f}};

        const std::vector<float> potentials = solve(pack(network), 4);
        CHECK(near(potentials[0], 10.0f));
        CHECK(near(potentials[1], 7.5f));
        CHECK(near(potentials[2], 0.0f));
    }

    // 2 mA into 1 kOhm to ground: Ohm's law.
    void current_source()
    {
        network network;
        network.nodes = 2;
        network.branches = {{0, 1, 1.0f / 1000.0f}};
        network.holds = {{1, 0.0f}};
        network.sources = {{0, 0.002f}};

        const std::vector<float> potentials = solve(pack(network), 4);
        CHECK(near(potentials[0], 2.0f));
    }

    // A chain takes many sweeps, and ends up where the currents balance.
    void chain()
    {
        const std::uint32_t nodes = 9;

        network network;
        network.nodes = nodes;
        network.holds = {{0, 8.0f}, {nodes - 1, 0.0f}};
        for (std::uint32_t i = 0; i + 1 < nodes; ++i)
            network.branches.push_back({i, i + 1, 0.5f});

        // Equal resistors: the potential falls in equal steps.
        const std::vector<float> potentials = solve(pack(network), 2000);
        for (std::uint32_t i = 0; i < nodes; ++i)
            CHECK(near(potentials[i], 8.0f - static_cast<float>(i), 1e-3f));
    }

    // A node nothing is connected to keeps its potential instead of
    // dividing by zero.
    void isolated()
    {
        network network;
        network.nodes = 1;

        const packed_network packed = pack(network);
        const std::vector<float> from{3.0f};
        std::vector<float> to{0.0f};
        relax(packed, from, to);
        CHECK(near(to[0], 3.0f));
    }
} // namespace

int main()
{
    packing();
    divider();
    current_source();
    chain();
    isolated();
    return volt::tests::result("simulation network");
}
