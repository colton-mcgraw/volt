#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// -------------------------------------------------------------------------
// network
// -------------------------------------------------------------------------
//
// What the simulation's first kernel solves: a network of conductances, with
// some nodes held at a potential and currents fed into others. That is the
// DC operating point of a circuit of resistors and ideal sources, and the
// linear system at the core of anything more.
//
// It is solved by relaxation: every node repeatedly takes the potential its
// neighbors pull it to. A sweep is independent per node, which is what makes
// it fit a compute shader (assets/shaders/simulation_relax.comp), and it
// converges for any network in which every node has a path to a held one.
// Slowly for large ones; a faster method is a matter of replacing the
// kernel, not of what surrounds it.
//
// Free of Vulkan on purpose, so it can be tested without a device.

namespace volt::simulation
{

    struct network
    {
        // A conductance between two nodes, in siemens: 1 / resistance.
        struct branch
        {
            std::uint32_t a = 0;
            std::uint32_t b = 0;
            float conductance = 0.0f;
        };

        // A node held at a potential in volts: ground, or the far end of an
        // ideal voltage source that is grounded on the other.
        struct held
        {
            std::uint32_t node = 0;
            float potential = 0.0f;
        };

        // A current in amperes fed into a node from outside.
        struct source
        {
            std::uint32_t node = 0;
            float current = 0.0f;
        };

        // Nodes are numbered from 0. Entries naming a node beyond the count
        // are ignored.
        std::uint32_t nodes = 0;
        std::vector<branch> branches;
        std::vector<held> holds;
        std::vector<source> sources;
    };

    // ---- the network as the kernel reads it --------------------------------
    //
    // Per node, the run of edges that leave it. A branch appears twice, once
    // from either end. The layouts are those of the shader's buffers.

    struct packed_node
    {
        std::uint32_t first = 0; // its edges are edges[first] to edges[first + count - 1]
        std::uint32_t count = 0;
        std::uint32_t held = 0;  // nonzero: `value` is its potential, and it stays there
        float value = 0.0f;      // otherwise the current fed into it
    };

    struct packed_edge
    {
        std::uint32_t other = 0;
        float conductance = 0.0f;
    };

    struct packed_network
    {
        std::vector<packed_node> nodes;
        std::vector<packed_edge> edges;
    };

    inline packed_network pack(const network &network)
    {
        packed_network packed;
        packed.nodes.resize(network.nodes);

        const auto valid = [&](const network::branch &branch) {
            return branch.a < network.nodes && branch.b < network.nodes && branch.a != branch.b;
        };

        for (const network::branch &branch : network.branches)
        {
            if (!valid(branch))
                continue;
            ++packed.nodes[branch.a].count;
            ++packed.nodes[branch.b].count;
        }

        std::uint32_t total = 0;
        for (packed_node &node : packed.nodes)
        {
            node.first = total;
            total += node.count;
            node.count = 0; // counted up again while the edges are placed
        }

        packed.edges.resize(total);
        for (const network::branch &branch : network.branches)
        {
            if (!valid(branch))
                continue;
            packed_node &a = packed.nodes[branch.a];
            packed_node &b = packed.nodes[branch.b];
            packed.edges[a.first + a.count++] = {branch.b, branch.conductance};
            packed.edges[b.first + b.count++] = {branch.a, branch.conductance};
        }

        for (const network::source &source : network.sources)
        {
            if (source.node < network.nodes)
                packed.nodes[source.node].value += source.current;
        }

        // After the sources: a held node has no use for a current.
        for (const network::held &held : network.holds)
        {
            if (held.node >= network.nodes)
                continue;
            packed.nodes[held.node].held = 1;
            packed.nodes[held.node].value = held.potential;
        }

        return packed;
    }

    // One sweep on the CPU: the reference for the shader, which has to stay
    // in agreement with it. Reads the potentials in `from`, writes `to`;
    // both hold one value per node and must not overlap.
    inline void relax(const packed_network &network, std::span<const float> from, std::span<float> to)
    {
        for (std::size_t index = 0; index < network.nodes.size(); ++index)
        {
            const packed_node &node = network.nodes[index];
            if (node.held != 0)
            {
                to[index] = node.value;
                continue;
            }

            // Kirchhoff's current law, solved for this node's potential.
            float current = node.value;
            float conductance = 0.0f;
            for (std::uint32_t i = node.first; i < node.first + node.count; ++i)
            {
                current += network.edges[i].conductance * from[network.edges[i].other];
                conductance += network.edges[i].conductance;
            }

            // Connected to nothing: keep what it had.
            to[index] = conductance > 0.0f ? current / conductance : from[index];
        }
    }

} // namespace volt::simulation
