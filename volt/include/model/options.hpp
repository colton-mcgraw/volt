#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// -------------------------------------------------------------------------
// options
// -------------------------------------------------------------------------
//
// How finely curved geometry is turned into triangles, and what a load has
// to say about what it could not read. Shared by every format; only STEP has
// curved geometry to tessellate.

namespace volt::model
{

    struct tessellation_options
    {
        // The largest distance, in millimeters, between a triangle and the
        // surface it stands for, and between an edge's segments and the
        // curve. Electronic parts are small (a 0402 resistor is 1 x 0.5 mm),
        // hence the fine default.
        double chord_tolerance = 0.01;

        // The largest angle in radians between the normals at the two ends
        // of a segment, or at the corners of a triangle. Keeps small radii
        // round: a pin 0.1 mm across is within any chord tolerance as a
        // triangle, but does not look round as one.
        double angle_tolerance = 0.3490659; // 20 degrees

        // A face that would need more points than this is tessellated
        // coarser than asked rather than without bound.
        std::size_t max_points_per_face = 50000;

        // Options scaled by one number: 1 is the default, 2 twice as fine
        // (half the tolerances), 0.5 half as fine.
        static tessellation_options with_quality(double quality);
    };

    struct load_options
    {
        tessellation_options tessellation;

        // AP242 files can carry both the exact B-rep of a solid and the
        // exporter's own tessellation of it. Use the tessellation where both
        // exist: it is what the exporter meant to be seen, and it is faster.
        bool prefer_tessellation = true;
    };

    // What a load did not manage. A model that loads with problems still
    // loads: the faces that could not be tessellated are left out, and this
    // says how many and why.
    struct load_report
    {
        std::size_t entities = 0; // STEP instances in the file

        std::size_t solids = 0; // solids, shells and tessellated items turned into meshes
        std::size_t faces = 0;  // faces that went into them
        std::size_t faces_failed = 0;

        // Entity types the file uses where geometry was expected and that
        // are not understood, with how often each was met.
        std::map<std::string, std::size_t, std::less<>> unsupported;

        // Problems worth a line each, at most max_warnings of them: which
        // face failed and why.
        std::vector<std::string> warnings;
        std::size_t warnings_dropped = 0;
        static constexpr std::size_t max_warnings = 100;

        // Wall-clock seconds spent reading the file and building the model.
        double parse_seconds = 0.0;
        double build_seconds = 0.0;

        void warn(std::string message);
        void unsupported_entity(std::string_view type);

        bool clean() const noexcept { return faces_failed == 0 && unsupported.empty() && warnings.empty(); }

        // A few lines for a log.
        std::string summary() const;
    };

} // namespace volt::model
