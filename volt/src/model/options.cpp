#include <model/options.hpp>

#include <algorithm>
#include <format>

namespace volt::model
{

    tessellation_options tessellation_options::with_quality(double quality)
    {
        tessellation_options options;
        const double q = std::clamp(quality, 0.01, 100.0);
        options.chord_tolerance /= q;
        options.angle_tolerance = std::clamp(options.angle_tolerance / q, 0.02, 1.2);
        return options;
    }

    void load_report::warn(std::string message)
    {
        if (warnings.size() < max_warnings)
            warnings.push_back(std::move(message));
        else
            ++warnings_dropped;
    }

    void load_report::unsupported_entity(std::string_view type)
    {
        auto it = unsupported.find(type);
        if (it == unsupported.end())
            unsupported.emplace(std::string(type), 1);
        else
            ++it->second;
    }

    std::string load_report::summary() const
    {
        std::string text = std::format("{} entities, {} solids, {} faces ({} failed); parse {:.3f} s, build {:.3f} s",
                                       entities, solids, faces, faces_failed, parse_seconds, build_seconds);
        for (const auto &[type, count] : unsupported)
            text += std::format("\n  unsupported {}: {}", type, count);
        for (const std::string &warning : warnings)
            text += std::format("\n  {}", warning);
        if (warnings_dropped != 0)
            text += std::format("\n  ... and {} more warnings", warnings_dropped);
        return text;
    }

} // namespace volt::model
