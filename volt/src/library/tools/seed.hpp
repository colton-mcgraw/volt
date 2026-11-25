#pragma once

#include <library/library.hpp>

#include <vector>

// The libraries shipped in assets/libraries, made here rather than by hand:
// several thousand parts that follow a few rules (the E series in every
// package, the voltage ratings each dielectric is made in, ...) are better
// generated, and regenerated when a rule changes. Each library is saved as
// <name>.voltlib.
//
// The manufacturer part numbers follow the manufacturers' published
// numbering schemes (Yageo RC and CC, Panasonic FR, Bourns SRR); check one
// against the datasheet before ordering it. The discrete and integrated
// parts are real, common parts.

namespace volt::library::seed
{

    std::vector<library> libraries();

} // namespace volt::library::seed
