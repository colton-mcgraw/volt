#pragma once

#include "bspline.hpp"
#include "step_access.hpp"

#include <math/vector.hpp>

#include <limits>
#include <memory>
#include <vector>

// Private to volt_model: the curves edges run along, evaluated in double in
// the file's own length unit. Each curve maps a parameter t to a point; the
// parameterizations are those of ISO 10303-42, so that parameter values in a
// file (trimming, for instance) mean what the file means.

namespace volt::model::geometry
{

    // How finely to follow a curve: the largest distance between a segment
    // and the curve, and the largest turn of the tangent along a segment.
    struct sampling
    {
        double chord = 0.01;
        double angle = 0.35;
        std::size_t max_points = 20000;
    };

    class curve
    {
    public:
        virtual ~curve() = default;

        virtual math::vec3d point(double t) const = 0;
        virtual math::vec3d derivative(double t) const = 0;

        // The parameter of the point on the curve closest to p.
        virtual double project(const math::vec3d &p) const;

        // Parameters strictly between t0 and t1 (either order) at which to
        // place points so that the polyline through them, from point(t0) to
        // point(t1), follows the curve within `sampling`.
        virtual void sample(double t0, double t1, const sampling &sampling, std::vector<double> &out) const;

        // The parameter range of the curve; one period for a closed curve.
        double start = -std::numeric_limits<double>::infinity();
        double end = std::numeric_limits<double>::infinity();
        bool periodic = false;
        double period = 0.0;

    protected:
        double newton(const math::vec3d &p, double t) const;
        void adaptive(double t0, double t1, int initial, const sampling &sampling, std::vector<double> &out) const;
    };

    class line_curve final : public curve
    {
    public:
        line_curve(const math::vec3d &origin, const math::vec3d &direction);
        math::vec3d point(double t) const override { return m_origin + m_direction * t; }
        math::vec3d derivative(double) const override { return m_direction; }
        double project(const math::vec3d &p) const override;
        void sample(double, double, const sampling &, std::vector<double> &) const override {}

    private:
        math::vec3d m_origin;
        math::vec3d m_direction; // with the vector's magnitude: t is in its units
    };

    // CIRCLE and ELLIPSE: t is the angle in radians from the frame's x axis.
    class ellipse_curve final : public curve
    {
    public:
        ellipse_curve(const step::frame &frame, double a, double b);
        math::vec3d point(double t) const override;
        math::vec3d derivative(double t) const override;
        double project(const math::vec3d &p) const override;
        void sample(double t0, double t1, const sampling &sampling, std::vector<double> &out) const override;

    private:
        step::frame m_frame;
        double m_a;
        double m_b;
    };

    // POLYLINE: t runs from 1 at the first point to n at the last, linearly
    // along each segment.
    class polyline_curve final : public curve
    {
    public:
        explicit polyline_curve(std::vector<math::vec3d> points);
        math::vec3d point(double t) const override;
        math::vec3d derivative(double t) const override;
        double project(const math::vec3d &p) const override;
        void sample(double t0, double t1, const sampling &sampling, std::vector<double> &out) const override;

    private:
        std::vector<math::vec3d> m_points;
    };

    // B_SPLINE_CURVE_WITH_KNOTS, rational or not.
    class bspline_curve final : public curve
    {
    public:
        // Weights empty for a polynomial curve.
        bspline_curve(bspline_basis basis, std::vector<math::vec3d> points, std::vector<double> weights);
        math::vec3d point(double t) const override;
        math::vec3d derivative(double t) const override;
        double project(const math::vec3d &p) const override;
        void sample(double t0, double t1, const sampling &sampling, std::vector<double> &out) const override;

        bool valid() const noexcept { return m_basis.valid(); }

    private:
        void evaluate(double t, math::vec3d &point, math::vec3d *derivative) const;

        bspline_basis m_basis;
        std::vector<math::vec3d> m_points;
        std::vector<double> m_weights;
    };

    // ---------------------------------------------------------------------
    // From STEP entities
    // ---------------------------------------------------------------------

    struct curve_result
    {
        std::shared_ptr<const curve> curve;
        std::string unsupported; // the entity type when there is no curve
    };

    // The 3D curve a STEP curve entity stands for. SURFACE_CURVE and
    // SEAM_CURVE give their 3D curve; TRIMMED_CURVE its basis curve (edges
    // trim by their vertices). `depth` guards against reference cycles.
    curve_result make_curve(const step::access &access, const part21::entity &entity, int depth = 0);

} // namespace volt::model::geometry
