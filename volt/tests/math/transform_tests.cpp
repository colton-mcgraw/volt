#include "check.hpp"

// <windows.h> defines `near` and `far` as macros. These stand in for it, so
// the test fails to compile if a math header ever uses either as a name.
#define near
#define far

#include <math/math.hpp>

#include <cmath>
#include <format>
#include <numbers>
#include <random>
#include <unordered_set>

using namespace volt::math;

constexpr double pi = std::numbers::pi;

// ---- compile-time checks -------------------------------------------------

static_assert(approx_equal(rotation(pi / 2) * vec2d{1.0, 0.0}, vec2d{0.0, 1.0}));
static_assert(approx_equal(rotation_z(pi / 2) * vec3d{1.0, 0.0, 0.0}, vec3d{0.0, 1.0, 0.0}));
static_assert(approx_equal(rotation_x(pi / 2) * vec3d{0.0, 1.0, 0.0}, vec3d{0.0, 0.0, 1.0}));
static_assert(approx_equal(rotation_y(pi / 2) * vec3d{0.0, 0.0, 1.0}, vec3d{1.0, 0.0, 0.0}));
static_assert(approx_equal(rotation(vec3i{0, 0, 5}, pi / 3), rotation_z(pi / 3)));
static_assert(scaling(vec3i{1, 2, 3}) == mat3i::from_diagonal(vec3i{1, 2, 3}) && scaling<2>(3) == mat2i::identity() * 3);
static_assert(transform_point(translation(vec2i{5, 6}), vec2i{1, 1}) == vec2i{6, 7});
static_assert(transform_direction(translation(vec2i{5, 6}), vec2i{1, 1}) == vec2i{1, 1});
static_assert(translation_part(affine(mat2i::identity() * 2, vec2i{5, 6})) == vec2i{5, 6});
static_assert(linear_part(affine(mat2i::identity() * 2, vec2i{5, 6})) == mat2i::identity() * 2);
static_assert(affine(mat2i::identity()) == mat3i::identity());
static_assert(radians(180.0) == pi && degrees(pi) == 180.0);

// Projections: near -> 0, far -> 1, and the frustum edges land on +-1.
constexpr auto proj = perspective(pi / 2, 2.0, 1.0, 100.0);
static_assert(approx_equal(transform_point(proj, vec3d{0.0, 0.0, -1.0}), vec3d{0.0, 0.0, 0.0}));
static_assert(approx_equal(transform_point(proj, vec3d{0.0, 0.0, -100.0}), vec3d{0.0, 0.0, 1.0}));
static_assert(approx_equal(transform_point(proj, vec3d{2.0, 1.0, -1.0}), vec3d{1.0, 1.0, 0.0}));
constexpr auto ortho = orthographic(-2.0, 6.0, -1.0, 3.0, 0.5, 10.5);
static_assert(approx_equal(transform_point(ortho, vec3d{-2.0, -1.0, -0.5}), vec3d{-1.0, -1.0, 0.0}));
static_assert(approx_equal(transform_point(ortho, vec3d{6.0, 3.0, -10.5}), vec3d{1.0, 1.0, 1.0}));
static_assert(orthographic(8.0, 4.0, 0.5, 10.5) == orthographic(-4.0, 4.0, -2.0, 2.0, 0.5, 10.5));

// look_at: the eye goes to the origin, the target onto -z, up stays up.
constexpr auto view = look_at(vec3i{0, 0, 5}, vec3i{0, 0, 0}, vec3i{0, 1, 0});
static_assert(approx_equal(transform_point(view, vec3d{0.0, 0.0, 5.0}), vec3d{}));
static_assert(approx_equal(transform_point(view, vec3d{0.0, 0.0, 0.0}), vec3d{0.0, 0.0, -5.0}));
static_assert(approx_equal(transform_direction(view, vec3d{0.0, 1.0, 0.0}), vec3d{0.0, 1.0, 0.0}));
static_assert(approx_equal(transform_direction(view, vec3d{1.0, 0.0, 0.0}), vec3d{1.0, 0.0, 0.0}));

// Quaternions agree with the matrices.
static_assert(quatd{} == quatd::identity() && quatd{}.to_matrix() == mat3d::identity());
constexpr auto qz = quatd::from_axis_angle(vec3i{0, 0, 2}, pi / 2);
static_assert(approx_equal(qz.rotate(vec3d{1.0, 0.0, 0.0}), vec3d{0.0, 1.0, 0.0}));
static_assert(approx_equal(qz.to_matrix(), rotation_z(pi / 2)));
static_assert(approx_equal(qz * qz, quatd::from_axis_angle(vec3d{0.0, 0.0, 1.0}, pi)));
static_assert(approx_equal(qz * qz.inverse(), quatd{}) && approx_equal(qz.conjugate(), qz.inverse()));
static_assert(approx_equal(quatd::from_rotation_matrix(rotation_z(pi / 2)), qz));
static_assert(approx_equal(nlerp(quatd{}, qz, 0.5), quatd::from_axis_angle(vec3d{0.0, 0.0, 1.0}, pi / 4), 1e-12));
static_assert(approx_equal(qz.axis(), vec3d{0.0, 0.0, 1.0}) && quatd{}.axis() == vec3d{1.0, 0.0, 0.0});

int main()
{
    std::mt19937 rng(2024);
    std::uniform_real_distribution<double> unit(-1.0, 1.0);
    std::uniform_real_distribution<double> turn(-2 * pi, 2 * pi);
    const auto random_axis = [&]
    {
        for (;;)
        {
            const vec3d v{unit(rng), unit(rng), unit(rng)};
            if (magnitude(v) > 0.1)
                return v;
        }
    };

    for (int round = 0; round < 2000; ++round)
    {
        const vec3d axis = random_axis();
        const double angle = turn(rng);
        const vec3d p{unit(rng), unit(rng), unit(rng)};

        const mat3d m = rotation(axis, angle);
        const quatd q = quatd::from_axis_angle(axis, angle);

        // Rotation matrices are orthogonal with determinant +1.
        CHECK(is_orthogonal(m, 1e-12));
        CHECK(approx_equal(determinant(m), 1.0, 1e-12));
        CHECK(approx_equal(inverse(m), transpose(m), 1e-12));

        // Matrix, quaternion and their conversions describe one rotation.
        CHECK(approx_equal(q.to_matrix(), m, 1e-12));
        CHECK(approx_equal(q.rotate(p), m * p, 1e-12));
        CHECK(approx_equal(rotate(q, p), m * p, 1e-12));
        const quatd back = quatd::from_rotation_matrix(m);
        CHECK(approx_equal(back, q, 1e-9) || approx_equal(back, -q, 1e-9));
        CHECK(approx_equal(q.magnitude(), 1.0, 1e-12));

        // axis() and angle() reproduce the quaternion, whatever the sign of w.
        CHECK(approx_equal(quatd::from_axis_angle(q.axis(), q.angle()), q, 1e-9));

        // Products compose like matrices: the right factor applies first.
        const vec3d axis2 = random_axis();
        const double angle2 = turn(rng);
        const quatd q2 = quatd::from_axis_angle(axis2, angle2);
        CHECK(approx_equal((q * q2).to_matrix(), m * rotation(axis2, angle2), 1e-12));
        CHECK(approx_equal((q * q2).rotate(p), q.rotate(q2.rotate(p)), 1e-12));

        // slerp: unit, hits both endpoints, and moves at constant speed.
        CHECK(approx_equal(slerp(q, q2, 0.0), q, 1e-12));
        const quatd end = slerp(q, q2, 1.0);
        CHECK(approx_equal(end, q2, 1e-9) || approx_equal(end, -q2, 1e-9));
        const quatd mid = slerp(q, q2, 0.5);
        CHECK(approx_equal(mid.magnitude(), 1.0, 1e-9));
        CHECK(approx_equal(std::abs(dot(q, mid)), std::abs(dot(mid, q2)), 1e-9));
        CHECK(approx_equal(nlerp(q, q2, 0.5).magnitude(), 1.0, 1e-12));

        // Affine transforms compose and invert.
        const vec3d offset{unit(rng), unit(rng), unit(rng)};
        const mat4d t = affine(m, offset);
        CHECK(approx_equal(transform_point(t, p), m * p + offset, 1e-12));
        CHECK(approx_equal(transform_direction(t, p), m * p, 1e-12));
        CHECK(approx_equal(transform_point(inverse(t), transform_point(t, p)), p, 1e-11));
        CHECK(approx_equal(translation(offset) * affine(m), t, 1e-12));
    }

    // look_at from random poses: an orthonormal view basis looking at the target.
    for (int round = 0; round < 500; ++round)
    {
        const vec3d eye = random_axis() * 10.0;
        const vec3d target = random_axis();
        const mat4d v = look_at(eye, target, vec3d{0.0, 1.0, 0.0});
        CHECK(is_orthogonal(linear_part(v), 1e-10));
        CHECK(approx_equal(transform_point(v, eye), vec3d{}, 1e-10));
        const vec3d seen = transform_point(v, target);
        CHECK(approx_equal(seen.x(), 0.0, 1e-10) && approx_equal(seen.y(), 0.0, 1e-10) && seen.z() < 0.0);
    }

    // float instantiations
    {
        const auto m = rotation(vec3f{1.0f, 1.0f, 0.0f}, 0.75f);
        const auto q = quatf::from_axis_angle(vec3f{1.0f, 1.0f, 0.0f}, 0.75f);
        CHECK(approx_equal(q.to_matrix(), m, 1e-6f));
        CHECK(approx_equal(q.angle(), 0.75f, 1e-6f));
        const mat4f p = perspective(1.0f, 1.5f, 0.1f, 50.0f);
        CHECK(approx_equal(transform_point(p, vec3f{0.0f, 0.0f, -50.0f}).z(), 1.0f, 1e-5f));
    }

    // std integration
    {
        CHECK(std::format("{}", quatd{}) == "(0, 0, 0; 1)");
        std::unordered_set<quatd> set{quatd{}, quatd{}, quatd{1.0, 0.0, 0.0, 0.0}};
        CHECK(set.size() == 2);
    }

    return volt::tests::result("transform_tests");
}
