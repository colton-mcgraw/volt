#pragma once

#include "bspline.hpp"
#include "curves.hpp"
#include "step_access.hpp"

#include <math/vector.hpp>

#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Private to volt_model: the surfaces faces lie on, evaluated in double in the
// file's own length unit. Each maps a parameter pair (u, v) to a point, with
// the parameterizations of ISO 10303-42, and has its normal along
// d/du x d/dv: the direction a face with same_sense .T. faces.

namespace volt::model::geometry
{

    class surface
    {
    public:
        virtual ~surface() = default;

        virtual math::vec3d point(const math::vec2d &uv) const = 0;
        virtual void derivatives(const math::vec2d &uv, math::vec3d &point, math::vec3d &du,
                                 math::vec3d &dv) const = 0;

        // Unit normal along du x dv. Where that vanishes (the pole of a
        // sphere, the apex of a cone) the limit from inside the domain.
        virtual math::vec3d normal(const math::vec2d &uv) const;

        // The parameters of the point on the surface closest to p. `hint`
        // is where a nearby point projected to; the answer stays close to it
        // where the surface wraps.
        virtual math::vec2d project(const math::vec3d &p, const math::vec2d *hint) const;

        // Whether the surface collapses to a point along `axis` at uv: the
        // derivative along it vanishes, and the parameter along it means
        // nothing there.
        virtual bool singular(const math::vec2d &uv, int axis) const;

        // Where on the other axis the surface collapses along `axis`, on the
        // upper or the lower side of the domain, if it does: the poles of a
        // sphere (v = +-pi/2 along u), the apex of a cone.
        virtual std::optional<double> pole(int axis, bool upper) const;

        bool periodic[2] = {false, false};
        double period[2] = {0.0, 0.0};

        // The parameter domain; infinite along the axis of a cylinder.
        double lo[2] = {-std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
        double hi[2] = {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};

        bool planar = false;

        // A length typical of the surface (a radius, the size of the control
        // net), for tolerances that are relative.
        double size = 1.0;

    protected:
        math::vec2d newton(const math::vec3d &p, math::vec2d uv) const;
        math::vec2d nearest_on_grid(const math::vec3d &p, int resolution) const;
        math::vec2d clamp(math::vec2d uv) const;
    };

    class plane_surface final : public surface
    {
    public:
        explicit plane_surface(const step::frame &frame);
        math::vec3d point(const math::vec2d &uv) const override;
        void derivatives(const math::vec2d &uv, math::vec3d &p, math::vec3d &du, math::vec3d &dv) const override;
        math::vec3d normal(const math::vec2d &) const override { return m_frame.z; }
        math::vec2d project(const math::vec3d &p, const math::vec2d *hint) const override;
        bool singular(const math::vec2d &, int) const override { return false; }
        std::optional<double> pole(int, bool) const override { return std::nullopt; }

    private:
        step::frame m_frame;
    };

    // CYLINDRICAL_SURFACE and CONICAL_SURFACE: u is the angle about the
    // axis, v the distance along it; the radius is r + v tan(semi_angle).
    class cone_surface final : public surface
    {
    public:
        cone_surface(const step::frame &frame, double radius, double semi_angle);
        math::vec3d point(const math::vec2d &uv) const override;
        void derivatives(const math::vec2d &uv, math::vec3d &p, math::vec3d &du, math::vec3d &dv) const override;
        math::vec3d normal(const math::vec2d &uv) const override;
        math::vec2d project(const math::vec3d &p, const math::vec2d *hint) const override;
        bool singular(const math::vec2d &uv, int axis) const override;
        std::optional<double> pole(int axis, bool upper) const override;

    private:
        step::frame m_frame;
        double m_radius;
        double m_tan;
    };

    // SPHERICAL_SURFACE: u the longitude, v the latitude in -pi/2..pi/2.
    class sphere_surface final : public surface
    {
    public:
        sphere_surface(const step::frame &frame, double radius);
        math::vec3d point(const math::vec2d &uv) const override;
        void derivatives(const math::vec2d &uv, math::vec3d &p, math::vec3d &du, math::vec3d &dv) const override;
        math::vec3d normal(const math::vec2d &uv) const override;
        math::vec2d project(const math::vec3d &p, const math::vec2d *hint) const override;
        bool singular(const math::vec2d &uv, int axis) const override;
        std::optional<double> pole(int axis, bool upper) const override;

    private:
        step::frame m_frame;
        double m_radius;
    };

    // TOROIDAL_SURFACE: u about the axis, v about the tube.
    class torus_surface final : public surface
    {
    public:
        torus_surface(const step::frame &frame, double major, double minor);
        math::vec3d point(const math::vec2d &uv) const override;
        void derivatives(const math::vec2d &uv, math::vec3d &p, math::vec3d &du, math::vec3d &dv) const override;
        math::vec3d normal(const math::vec2d &uv) const override;
        math::vec2d project(const math::vec3d &p, const math::vec2d *hint) const override;
        bool singular(const math::vec2d &uv, int axis) const override;
        std::optional<double> pole(int, bool) const override { return std::nullopt; }

    private:
        step::frame m_frame;
        double m_major;
        double m_minor;
    };

    // SURFACE_OF_LINEAR_EXTRUSION: the curve at u, moved v times the vector.
    class extrusion_surface final : public surface
    {
    public:
        extrusion_surface(std::shared_ptr<const curve> curve, const math::vec3d &direction);
        math::vec3d point(const math::vec2d &uv) const override;
        void derivatives(const math::vec2d &uv, math::vec3d &p, math::vec3d &du, math::vec3d &dv) const override;
        math::vec2d project(const math::vec3d &p, const math::vec2d *hint) const override;
        std::optional<double> pole(int, bool) const override { return std::nullopt; }

    private:
        std::shared_ptr<const curve> m_curve;
        math::vec3d m_direction;
    };

    // SURFACE_OF_REVOLUTION: the curve at v, turned by u about the axis.
    class revolution_surface final : public surface
    {
    public:
        revolution_surface(std::shared_ptr<const curve> curve, const math::vec3d &origin, const math::vec3d &axis);
        math::vec3d point(const math::vec2d &uv) const override;
        void derivatives(const math::vec2d &uv, math::vec3d &p, math::vec3d &du, math::vec3d &dv) const override;
        math::vec2d project(const math::vec3d &p, const math::vec2d *hint) const override;
        std::optional<double> pole(int axis, bool upper) const override;

    private:
        math::vec3d rotate(const math::vec3d &v, double angle) const;

        std::shared_ptr<const curve> m_curve;
        math::vec3d m_origin;
        math::vec3d m_axis;
        math::vec3d m_reference; // the direction from the axis to the curve, at u = 0
    };

    // B_SPLINE_SURFACE_WITH_KNOTS, rational or not. Control points are
    // indexed [i][j], i along u.
    class bspline_surface final : public surface
    {
    public:
        bspline_surface(bspline_basis u, bspline_basis v, std::vector<math::vec3d> points, std::vector<double> weights);
        math::vec3d point(const math::vec2d &uv) const override;
        void derivatives(const math::vec2d &uv, math::vec3d &p, math::vec3d &du, math::vec3d &dv) const override;
        math::vec2d project(const math::vec3d &p, const math::vec2d *hint) const override;

        bool valid() const noexcept { return m_valid; }

    private:
        bspline_basis m_u;
        bspline_basis m_v;
        std::vector<math::vec3d> m_points; // m_u.count() x m_v.count(), row i is along v
        std::vector<double> m_weights;
        bool m_valid = false;
        int m_grid = 16;
    };

    // ---------------------------------------------------------------------
    // From STEP entities
    // ---------------------------------------------------------------------

    struct surface_result
    {
        std::shared_ptr<const surface> surface;
        std::string unsupported;
    };

    // `angle_unit` is radians per angle unit of the representation the
    // surface belongs to (the semi-angle of a cone is in it).
    surface_result make_surface(const step::access &access, const part21::entity &entity, double angle_unit,
                                int depth = 0);

} // namespace volt::model::geometry
