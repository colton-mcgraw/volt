#pragma once

#include <cstddef>
#include <vector>

// Private to volt_model: B-spline basis functions along one parameter, for
// the B-spline curves and surfaces of STEP files. The algorithms are those of
// Piegl and Tiller, "The NURBS Book" (A2.1 and A2.2), with the first
// derivative taken from the basis of one degree lower.

namespace volt::model::geometry
{

    inline constexpr int max_degree = 25;

    class bspline_basis
    {
    public:
        // `knots` expanded (each knot repeated by its multiplicity). Valid
        // when knots.size() == count + degree + 1, the knots do not decrease
        // and the domain is not empty.
        bspline_basis() = default;
        bspline_basis(int degree, std::vector<double> knots, int count);

        bool valid() const noexcept { return m_valid; }
        int degree() const noexcept { return m_degree; }
        int count() const noexcept { return m_count; }
        const std::vector<double> &knots() const noexcept { return m_knots; }

        double start() const noexcept { return m_knots[static_cast<std::size_t>(m_degree)]; }
        double end() const noexcept { return m_knots[static_cast<std::size_t>(m_count)]; }

        // The knot span containing t: knots[span] <= t < knots[span + 1],
        // clamped to the domain.
        int span(double t) const noexcept;

        // The degree + 1 basis functions that are nonzero at t (those of
        // control points span - degree to span) and their first
        // derivatives. Both arrays must hold max_degree + 1 values.
        void evaluate(int span, double t, double *values, double *derivatives) const noexcept;

        // The distinct knots inside the domain, start and end included.
        std::vector<double> breaks() const;

    private:
        void basis(int span, double t, int degree, double *values) const noexcept;

        int m_degree = 0;
        int m_count = 0;
        std::vector<double> m_knots;
        bool m_valid = false;
    };

} // namespace volt::model::geometry
