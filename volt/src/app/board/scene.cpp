#include <app/board/scene.hpp>

#include <math/geometry.hpp>
#include <math/transform.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace volt::board
{
    namespace
    {
        // The camera's frame as look_at() builds it.
        struct frame
        {
            math::vec3f forward;
            math::vec3f right;
            math::vec3f up;
        };

        frame frame_of(const camera &camera) noexcept
        {
            frame f;
            f.forward = math::normalized(camera.target - camera.eye);
            f.right = math::normalized(math::cross(f.forward, camera.up));
            f.up = math::cross(f.right, f.forward);
            return f;
        }

        math::vec3f corner(const model::bounds &b, int index) noexcept
        {
            return {(index & 1) != 0 ? b.max[0] : b.min[0], (index & 2) != 0 ? b.max[1] : b.min[1],
                    (index & 4) != 0 ? b.max[2] : b.min[2]};
        }

        // A bar `width` thick from `a` to `b`, reaching half its width past
        // either end so that bars meeting at a corner close it: upright
        // when it runs mostly along z, else lying, turned about z.
        box segment(const math::vec3f &a, const math::vec3f &b, float width, const math::vec3f &color)
        {
            const math::vec3f d = b - a;
            const float length = std::sqrt(math::dot(d, d));
            box result;
            result.center = (a + b) * 0.5f;
            result.color = color;
            // Along x, y or z: the outline's edges are axis aligned.
            if (std::abs(d[2]) > std::abs(d[0]) && std::abs(d[2]) > std::abs(d[1]))
            {
                result.size = {width, width, length + width};
            }
            else
            {
                result.size = {length + width, width, width};
                result.rotation = std::atan2(d[1], d[0]);
            }
            return result;
        }
    } // namespace

    // ---- camera -----------------------------------------------------------------

    float camera::ortho_height() const noexcept
    {
        const math::vec3f d = target - eye;
        return 2.0f * std::sqrt(math::dot(d, d)) * std::tan(fov_y * 0.5f);
    }

    math::mat4f view_projection(const camera &camera, float aspect) noexcept
    {
        math::mat4f projection;
        if (camera.orthographic)
        {
            const float height = camera.ortho_height();
            projection = math::orthographic(height * aspect, height, -camera.z_far, camera.z_far);
        }
        else
        {
            projection = math::perspective(camera.fov_y, aspect, camera.z_near, camera.z_far);
        }

        // The projection's y points up, the image's down.
        for (std::size_t column = 0; column < 4; ++column)
            projection(1, column) = -projection(1, column);
        return projection * math::look_at(camera.eye, camera.target, camera.up);
    }

    const char *kind_name(object_kind kind) noexcept
    {
        switch (kind)
        {
        case object_kind::board:
            return "Board";
        case object_kind::part_body:
            return "Part body";
        case object_kind::part_model:
            return "Part model";
        case object_kind::dropped_model:
            return "Dropped model";
        }
        return "Object";
    }

    model::bounds bounds_of(const box &b) noexcept
    {
        const math::mat4f m = math::affine(math::rotation_z(b.rotation) * math::scaling(b.size), b.center);
        model::bounds unit;
        unit.min = {-0.5f, -0.5f, -0.5f};
        unit.max = {0.5f, 0.5f, 0.5f};
        model::bounds result;
        for (int i = 0; i < 8; ++i)
            result.add(math::transform_point(m, corner(unit, i)));
        return result;
    }

    // ---- objects ----------------------------------------------------------------

    object_id scene::add_object(object o)
    {
        o.id = next_object++;
        objects.push_back(std::move(o));
        return objects.back().id;
    }

    object *scene::find(object_id id) noexcept
    {
        if (id == 0)
            return nullptr;
        const auto at = std::ranges::lower_bound(objects, id, {}, &object::id);
        return at != objects.end() && at->id == id ? &*at : nullptr;
    }

    const object *scene::find(object_id id) const noexcept
    {
        return const_cast<scene *>(this)->find(id);
    }

    bool scene::drawn(object_id id) const noexcept
    {
        if (id == 0)
            return true;
        const object *o = find(id);
        if (o == nullptr)
            return true; // a table that lost track of it should not hide it
        return o->visible && shown[static_cast<std::size_t>(o->kind)];
    }

    std::size_t scene::count(object_kind kind) const noexcept
    {
        return static_cast<std::size_t>(std::ranges::count(objects, kind, &object::kind));
    }

    std::size_t adopt_boxes(scene &scene)
    {
        // The board: of the boxes with no object whose top is at z = 0, the
        // one that covers the most.
        int board = -1;
        float covered = 0.0f;
        for (std::size_t i = 0; i < scene.boxes.size(); ++i)
        {
            const box &b = scene.boxes[i];
            const float top = b.center[2] + b.size[2] * 0.5f;
            const float area = b.size[0] * b.size[1];
            if (b.object == 0 && std::abs(top) < 1.0e-3f && b.center[2] < 0.0f && area > covered)
            {
                board = static_cast<int>(i);
                covered = area;
            }
        }

        std::size_t made = 0;
        for (std::size_t i = 0; i < scene.boxes.size(); ++i)
        {
            box &b = scene.boxes[i];
            if (b.object != 0)
                continue;
            object o;
            o.kind = static_cast<int>(i) == board ? object_kind::board : object_kind::part_body;
            o.name = !b.name.empty()                 ? b.name
                     : o.kind == object_kind::board ? std::string("Board")
                                                    : std::string("Part body");
            o.position = {b.center[0], b.center[1], o.kind == object_kind::board ? 0.0f : b.center[2] - b.size[2] * 0.5f};
            o.rotation = b.rotation;
            o.bounds = bounds_of(b);
            o.triangles = 12;
            b.object = scene.add_object(std::move(o));
            ++made;
        }
        return made;
    }

    bool move_object(scene &scene, object_id id, const math::vec2f &offset)
    {
        object *o = scene.find(id);
        if (o == nullptr)
            return false;

        const math::vec3f by{offset.x(), offset.y(), 0.0f};
        const math::mat4f shift = math::translation(by);
        for (box &b : scene.boxes)
        {
            if (b.object == id)
                b.center += by;
        }
        for (mesh_instance &m : scene.meshes)
        {
            if (m.object == id)
                m.transform = shift * m.transform;
        }
        o->position += by;
        if (!o->bounds.empty())
        {
            o->bounds.min += by;
            o->bounds.max += by;
        }
        return true;
    }

    bool remove_object(scene &scene, object_id id)
    {
        object *o = scene.find(id);
        if (o == nullptr)
            return false;
        std::erase_if(scene.boxes, [id](const box &b) { return b.object == id; });
        std::erase_if(scene.meshes, [id](const mesh_instance &m) { return m.object == id; });
        scene.objects.erase(scene.objects.begin() + (o - scene.objects.data()));
        if (scene.selected == id)
            scene.selected = 0;
        return true;
    }

    void outline(std::vector<box> &marks, const model::bounds &bounds, float width, const math::vec3f &color)
    {
        if (bounds.empty())
            return;
        // Corners differ in one coordinate along each edge: the pairs whose
        // indices differ in one bit.
        for (int a = 0; a < 8; ++a)
        {
            for (int bit = 1; bit < 8; bit <<= 1)
            {
                if ((a & bit) == 0)
                    marks.push_back(segment(corner(bounds, a), corner(bounds, a | bit), width, color));
            }
        }
    }

    void bar(std::vector<box> &marks, const math::vec2f &a, const math::vec2f &b, float height, float width,
             const math::vec3f &color)
    {
        box result = segment({a.x(), a.y(), height}, {b.x(), b.y(), height}, width, color);
        result.size[0] -= width; // ends where the points are
        result.size[0] = std::max(result.size[0], width * 0.25f);
        marks.push_back(result);
    }

    // ---- picking ----------------------------------------------------------------

    std::optional<ray> view_ray(const camera &camera, const math::vec2f &pixel, const math::vec2f &size) noexcept
    {
        if (size.x() < 1.0f || size.y() < 1.0f)
            return std::nullopt;

        // The pixel as a point of the image, -1 to 1 either way, y up.
        const frame f = frame_of(camera);
        const float aspect = size.x() / size.y();
        const float x = 2.0f * pixel.x() / size.x() - 1.0f;
        const float y = 1.0f - 2.0f * pixel.y() / size.y();

        if (camera.orthographic)
        {
            const float half = camera.ortho_height() * 0.5f;
            const math::vec3f on_eye = camera.eye + f.right * (x * half * aspect) + f.up * (y * half);
            return ray{on_eye - f.forward * camera.z_far, f.forward};
        }

        // Spread as perspective() spreads the view.
        const float reach = std::tan(camera.fov_y * 0.5f);
        const math::vec3f direction = f.forward + f.right * (x * reach * aspect) + f.up * (y * reach);
        return ray{camera.eye, math::normalized(direction)};
    }

    std::optional<float> hit(const ray &r, const model::bounds &bounds) noexcept
    {
        if (bounds.empty())
            return std::nullopt;

        // The slabs: where the ray is between each pair of planes.
        float enter = 0.0f;
        float leave = std::numeric_limits<float>::max();
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            const float origin = r.origin[axis];
            const float direction = r.direction[axis];
            if (std::abs(direction) < 1.0e-12f)
            {
                if (origin < bounds.min[axis] || origin > bounds.max[axis])
                    return std::nullopt;
                continue;
            }
            float t0 = (bounds.min[axis] - origin) / direction;
            float t1 = (bounds.max[axis] - origin) / direction;
            if (t0 > t1)
                std::swap(t0, t1);
            enter = std::max(enter, t0);
            leave = std::min(leave, t1);
            if (enter > leave)
                return std::nullopt;
        }
        return enter;
    }

    std::optional<float> hit(const ray &r, const box &b) noexcept
    {
        // Into the box's own frame: about its centre, turned back.
        const float c = std::cos(-b.rotation);
        const float s = std::sin(-b.rotation);
        const math::vec3f o = r.origin - b.center;
        const ray local{{c * o[0] - s * o[1], s * o[0] + c * o[1], o[2]},
                        {c * r.direction[0] - s * r.direction[1], s * r.direction[0] + c * r.direction[1],
                         r.direction[2]}};
        model::bounds around;
        around.min = b.size * -0.5f;
        around.max = b.size * 0.5f;
        return hit(local, around);
    }

    std::optional<float> hit(const ray &r, const mesh_instance &instance) noexcept
    {
        if (!instance.shape)
            return std::nullopt;

        // Into the mesh's coordinates. The linear part is a scaled rotation
        // (perhaps mirrored), whose inverse is its transpose over the scale
        // squared. An affine map keeps the ray's parameter, so the distance
        // found there is the distance here.
        const math::mat4f &m = instance.transform;
        const float scale2 = m(0, 0) * m(0, 0) + m(1, 0) * m(1, 0) + m(2, 0) * m(2, 0);
        if (!(scale2 > 1.0e-20f))
            return std::nullopt;
        const math::vec3f moved = r.origin - math::vec3f{m(0, 3), m(1, 3), m(2, 3)};
        math::vec3f origin{};
        math::vec3f direction{};
        for (std::size_t i = 0; i < 3; ++i)
        {
            origin[i] = (m(0, i) * moved[0] + m(1, i) * moved[1] + m(2, i) * moved[2]) / scale2;
            direction[i] =
                (m(0, i) * r.direction[0] + m(1, i) * r.direction[1] + m(2, i) * r.direction[2]) / scale2;
        }

        // Every triangle, from either side (Moller and Trumbore).
        const pick_shape &shape = *instance.shape;
        std::optional<float> nearest;
        const std::size_t count = shape.positions.size();
        for (std::size_t t = 0; t + 2 < shape.indices.size(); t += 3)
        {
            const std::uint32_t ia = shape.indices[t];
            const std::uint32_t ib = shape.indices[t + 1];
            const std::uint32_t ic = shape.indices[t + 2];
            if (ia >= count || ib >= count || ic >= count)
                continue;
            const math::vec3f &a = shape.positions[ia];
            const math::vec3f e1 = shape.positions[ib] - a;
            const math::vec3f e2 = shape.positions[ic] - a;
            const math::vec3f p = math::cross(direction, e2);
            const float det = math::dot(e1, p);
            if (std::abs(det) < 1.0e-12f)
                continue;
            const float inv = 1.0f / det;
            const math::vec3f to = origin - a;
            const float u = math::dot(to, p) * inv;
            if (u < 0.0f || u > 1.0f)
                continue;
            const math::vec3f q = math::cross(to, e1);
            const float v = math::dot(direction, q) * inv;
            if (v < 0.0f || u + v > 1.0f)
                continue;
            const float along = math::dot(e2, q) * inv;
            if (along >= 0.0f && (!nearest || along < *nearest))
                nearest = along;
        }
        return nearest;
    }

    object_id pick(const scene &scene, const ray &r) noexcept
    {
        object_id best = 0;
        float nearest = std::numeric_limits<float>::max();
        float best_size = std::numeric_limits<float>::max();
        for (const object &o : scene.objects)
        {
            if (!scene.drawn(o.id))
                continue;
            auto t = hit(r, o.bounds);
            if (!t)
                continue;

            // A candidate: tested as it is drawn, where that is known.
            bool tested = false;
            std::optional<float> exact;
            const auto take = [&exact](std::optional<float> at) {
                if (at && (!exact || *at < *exact))
                    exact = at;
            };
            for (const box &b : scene.boxes)
            {
                if (b.object == o.id)
                {
                    tested = true;
                    take(hit(r, b));
                }
            }
            for (const mesh_instance &m : scene.meshes)
            {
                if (m.object == o.id && m.shape)
                {
                    tested = true;
                    take(hit(r, m));
                }
            }
            if (tested)
            {
                if (!exact)
                    continue; // near it, but not on it
                t = exact;
            }

            // The ray may start inside several boxes (a part on the board,
            // seen from close by): then the smaller one is meant.
            const math::vec3f s = o.bounds.size();
            const float size = s[0] * s[1] + s[1] * s[2] + s[0] * s[2];
            if (*t < nearest || (*t == nearest && size < best_size))
            {
                best = o.id;
                nearest = *t;
                best_size = size;
            }
        }
        return best;
    }

    std::optional<math::vec2f> point_on_board(const camera &camera, const math::vec2f &pixel,
                                              const math::vec2f &size) noexcept
    {
        const auto r = view_ray(camera, pixel, size);
        if (!r || !(r->direction[2] < -1.0e-6f))
            return std::nullopt;

        const float along = -r->origin[2] / r->direction[2];
        if (!(along > 0.0f))
            return std::nullopt;
        const math::vec3f at = r->origin + r->direction * along;
        return math::vec2f{at[0], at[1]};
    }

} // namespace volt::board
