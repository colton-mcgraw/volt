#include "bspline.hpp"

#include <algorithm>
#include <cmath>

namespace volt::model::geometry
{

    bspline_basis::bspline_basis(int degree, std::vector<double> knots, int count)
        : m_degree(degree), m_count(count), m_knots(std::move(knots))
    {
        m_valid = degree >= 1 && degree <= max_degree && count > degree &&
                  m_knots.size() == static_cast<std::size_t>(count + degree + 1);
        if (!m_valid)
            return;
        for (std::size_t i = 1; i < m_knots.size(); ++i)
        {
            if (!std::isfinite(m_knots[i]) || m_knots[i] < m_knots[i - 1])
            {
                m_valid = false;
                return;
            }
        }
        m_valid = end() > start();
    }

    int bspline_basis::span(double t) const noexcept
    {
        const int n = m_count - 1;
        if (t >= m_knots[static_cast<std::size_t>(n + 1)])
        {
            // The last span that is not empty.
            int s = n;
            while (s > m_degree && m_knots[static_cast<std::size_t>(s)] >= m_knots[static_cast<std::size_t>(s + 1)])
                --s;
            return s;
        }
        if (t <= m_knots[static_cast<std::size_t>(m_degree)])
        {
            int s = m_degree;
            while (s < n && m_knots[static_cast<std::size_t>(s + 1)] <= m_knots[static_cast<std::size_t>(m_degree)])
                ++s;
            return s;
        }
        // Binary search: the last knot not greater than t.
        const auto first = m_knots.begin() + m_degree;
        const auto last = m_knots.begin() + n + 1;
        const auto it = std::upper_bound(first, last, t);
        return static_cast<int>(it - m_knots.begin()) - 1;
    }

    void bspline_basis::basis(int span, double t, int degree, double *values) const noexcept
    {
        double left[max_degree + 1];
        double right[max_degree + 1];
        values[0] = 1.0;
        for (int j = 1; j <= degree; ++j)
        {
            left[j] = t - m_knots[static_cast<std::size_t>(span + 1 - j)];
            right[j] = m_knots[static_cast<std::size_t>(span + j)] - t;
            double saved = 0.0;
            for (int r = 0; r < j; ++r)
            {
                const double denominator = right[r + 1] + left[j - r];
                const double temp = denominator != 0.0 ? values[r] / denominator : 0.0;
                values[r] = saved + right[r + 1] * temp;
                saved = left[j - r] * temp;
            }
            values[j] = saved;
        }
    }

    void bspline_basis::evaluate(int span, double t, double *values, double *derivatives) const noexcept
    {
        const int p = m_degree;
        basis(span, t, p, values);

        double lower[max_degree + 1];
        basis(span, t, p - 1, lower);
        for (int j = 0; j <= p; ++j)
        {
            const int i = span - p + j;
            double d = 0.0;
            if (j >= 1)
            {
                const double denominator =
                    m_knots[static_cast<std::size_t>(i + p)] - m_knots[static_cast<std::size_t>(i)];
                if (denominator != 0.0)
                    d += p * lower[j - 1] / denominator;
            }
            if (j <= p - 1)
            {
                const double denominator =
                    m_knots[static_cast<std::size_t>(i + p + 1)] - m_knots[static_cast<std::size_t>(i + 1)];
                if (denominator != 0.0)
                    d -= p * lower[j] / denominator;
            }
            derivatives[j] = d;
        }
    }

    std::vector<double> bspline_basis::breaks() const
    {
        std::vector<double> result;
        for (int i = m_degree; i <= m_count; ++i)
        {
            const double k = m_knots[static_cast<std::size_t>(i)];
            if (result.empty() || k > result.back())
                result.push_back(k);
        }
        return result;
    }

} // namespace volt::model::geometry
