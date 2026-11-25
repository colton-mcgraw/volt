#include "math/check.hpp"

#include <app/board/model.hpp>
#include <app/board/scene.hpp>
#include <app/ui/windows/board_view.hpp>

#include <math/transform.hpp>

#include <cmath>
#include <memory>
#include <numbers>
#include <optional>
#include <vector>

// The board editor's logic that needs no GPU: the projection and the rays
// picking undoes it with, in perspective and parallel; picking objects by
// their bounds, hidden ones left out; the demo board's boxes adopted as
// objects; moving and removing an object; the marks; and the view
// controller telling clicks, orbits and grabs apart.

using namespace volt;

namespace
{
    bool near(float a, float b, float tolerance = 1.0e-3f)
    {
        return std::abs(a - b) <= tolerance;
    }

    // Where world point `p` lands in a view `size` big, as the renderer
    // projects it.
    math::vec2f project(const board::camera &camera, const math::vec3f &p, const math::vec2f &size)
    {
        const math::mat4f m = board::view_projection(camera, size.x() / size.y());
        const math::vec4f clip = m * math::vec4f{p[0], p[1], p[2], 1.0f};
        return {(clip[0] / clip[3] + 1.0f) * 0.5f * size.x(), (clip[1] / clip[3] + 1.0f) * 0.5f * size.y()};
    }

    void rays_undo_the_projection()
    {
        const math::vec2f size{640.0f, 400.0f};
        for (const bool parallel : {false, true})
        {
            board::camera camera;
            camera.eye = {55.0f, -95.0f, 80.0f};
            camera.orthographic = parallel;

            // A point anywhere, its pixel, and the ray through that pixel
            // passes through the point.
            for (const math::vec3f &p : {math::vec3f{0, 0, 0}, math::vec3f{-28, -12, 3}, math::vec3f{31, 14, 9}})
            {
                const math::vec2f pixel = project(camera, p, size);
                const auto r = board::view_ray(camera, pixel, size);
                CHECK(r.has_value());
                if (!r)
                    continue;
                const math::vec3f to = p - r->origin;
                const float along = math::dot(to, r->direction);
                const math::vec3f off = to - r->direction * along;
                CHECK(along > 0.0f);
                CHECK(std::sqrt(math::dot(off, off)) < 0.01f);

                if (p[2] == 0.0f)
                {
                    const auto on = board::point_on_board(camera, pixel, size);
                    CHECK(on.has_value() && near(on->x(), p[0], 0.01f) && near(on->y(), p[1], 0.01f));
                }
            }

            // In parallel every ray runs the same way.
            if (parallel)
            {
                const auto a = board::view_ray(camera, {0.0f, 0.0f}, size);
                const auto b = board::view_ray(camera, {600.0f, 380.0f}, size);
                CHECK(a && b && near(math::dot(a->direction, b->direction), 1.0f));
            }
        }

        // A parallel view is as tall as the perspective one at the target.
        board::camera camera;
        camera.eye = {0.0f, 0.0f, 100.0f};
        camera.up = {0.0f, 1.0f, 0.0f};
        CHECK(near(camera.ortho_height(), 2.0f * 100.0f * std::tan(camera.fov_y * 0.5f)));
        CHECK(!board::view_ray(camera, {1.0f, 1.0f}, {0.0f, 10.0f}).has_value());
    }

    void hits()
    {
        model::bounds b;
        b.min = {-1.0f, -1.0f, 0.0f};
        b.max = {1.0f, 1.0f, 2.0f};

        // Straight down onto the top face, from 10 above.
        const board::ray down{{0.0f, 0.0f, 12.0f}, {0.0f, 0.0f, -1.0f}};
        const auto t = board::hit(down, b);
        CHECK(t.has_value() && near(*t, 10.0f));

        // Beside it, parallel to a face: a miss. From inside: 0.
        CHECK(!board::hit({{3.0f, 0.0f, 12.0f}, {0.0f, 0.0f, -1.0f}}, b).has_value());
        CHECK(!board::hit({{0.0f, 0.0f, 12.0f}, {0.0f, 0.0f, 1.0f}}, b).has_value());
        const auto inside = board::hit({{0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}}, b);
        CHECK(inside.has_value() && *inside == 0.0f);
        CHECK(!board::hit(down, model::bounds{}).has_value());
    }

    // The demo board's shape: the board, a part body, and a model.
    board::scene sample_scene(board::model_meshes &meshes)
    {
        board::scene scene;
        scene.boxes.push_back({{0.0f, 0.0f, -0.8f}, {80.0f, 50.0f, 1.6f}, 0.0f, {0, 1, 0}});
        scene.boxes.push_back({{-8.0f, 2.0f, 0.7f}, {14.0f, 14.0f, 1.4f}, 0.0f, {0, 0, 0}});
        scene.boxes.push_back({{16.0f, -10.0f, 0.6f}, {5.0f, 4.0f, 1.2f}, 0.7f, {0, 0, 0}, 0, "U2 Regulator"});

        meshes.name = "chip";
        meshes.parts = {{3, 1}, {4, 1}};
        meshes.colors = {{1, 0, 0}, {0, 0, 1}};
        meshes.instances = {{0, math::mat4f::identity()}, {1, math::translation(math::vec3f{0.0f, 0.0f, 1.0f})}};
        meshes.bounds.min = {-2.0f, -2.0f, 0.0f};
        meshes.bounds.max = {2.0f, 2.0f, 2.0f};
        meshes.triangles = 40;
        return scene;
    }

    void adopting_and_picking()
    {
        board::model_meshes meshes;
        board::scene scene = sample_scene(meshes);

        // The boxes become the board and two part bodies; a second time
        // makes nothing.
        CHECK(board::adopt_boxes(scene) == 3);
        CHECK(board::adopt_boxes(scene) == 0);
        CHECK(scene.objects.size() == 3);
        CHECK(scene.count(board::object_kind::board) == 1 && scene.count(board::object_kind::part_body) == 2);
        const board::object_id board_id = scene.boxes[0].object;
        const board::object_id body_id = scene.boxes[1].object;

        // A box's name names its object; without one, its kind does.
        CHECK(scene.find(body_id) != nullptr && scene.find(body_id)->name == "Part body");
        CHECK(scene.find(scene.boxes[2].object) != nullptr && scene.find(scene.boxes[2].object)->name == "U2 Regulator");
        CHECK(scene.find(board_id) != nullptr && scene.find(board_id)->name == "Board");
        CHECK(scene.find(board_id) != nullptr && scene.find(board_id)->kind == board::object_kind::board);
        CHECK(near(scene.find(board_id)->bounds.max[2], 0.0f) && near(scene.find(board_id)->bounds.min[0], -40.0f));

        // A turned box's bounds hold its turned corners.
        const board::object *turned = scene.find(scene.boxes[2].object);
        CHECK(turned != nullptr && turned->bounds.max[0] - turned->bounds.min[0] > 5.0f);

        // A model: one object, its instances tagged, its bounds placed.
        const board::object_id chip = board::add_model(scene, meshes, math::translation(math::vec3f{20.0f, 10.0f, 0.0f}),
                                                       board::object_kind::dropped_model, "chip.step");
        CHECK(chip != 0 && scene.meshes.size() == 2);
        CHECK(scene.meshes[0].object == chip && scene.meshes[1].object == chip);
        const board::object *o = scene.find(chip);
        CHECK(o != nullptr && o->file == "chip.step" && o->triangles == 40 && near(o->position[0], 20.0f));
        CHECK(o != nullptr && near(o->bounds.min[0], 18.0f) && near(o->bounds.max[1], 12.0f));

        // Looking straight down: the part on top wins over the board under it.
        const auto down = [](float x, float y) { return board::ray{{x, y, 50.0f}, {0.0f, 0.0f, -1.0f}}; };
        CHECK(board::pick(scene, down(-8.0f, 2.0f)) == body_id);
        CHECK(board::pick(scene, down(20.0f, 10.0f)) == chip);
        CHECK(board::pick(scene, down(-30.0f, -20.0f)) == board_id);
        CHECK(board::pick(scene, down(100.0f, 0.0f)) == 0);

        // Hidden, by itself or by its kind, it is not picked: what is under it is.
        CHECK(scene.drawn(body_id) && scene.drawn(0));
        scene.find(body_id)->visible = false;
        CHECK(!scene.drawn(body_id));
        CHECK(board::pick(scene, down(-8.0f, 2.0f)) == board_id);
        scene.find(body_id)->visible = true;
        scene.shown[static_cast<std::size_t>(board::object_kind::dropped_model)] = false;
        CHECK(board::pick(scene, down(20.0f, 10.0f)) == board_id);
        scene.shown[static_cast<std::size_t>(board::object_kind::dropped_model)] = true;

        // Moving takes everything of it along.
        scene.selected = chip;
        CHECK(board::move_object(scene, chip, {5.0f, -2.0f}));
        o = scene.find(chip);
        CHECK(o != nullptr && near(o->position[0], 25.0f) && near(o->position[1], 8.0f));
        CHECK(o != nullptr && near(o->bounds.min[0], 23.0f) && near(o->bounds.max[1], 10.0f));
        CHECK(near(scene.meshes[1].transform(0, 3), 25.0f) && near(scene.meshes[1].transform(2, 3), 1.0f));
        CHECK(board::pick(scene, down(25.0f, 8.0f)) == chip);
        CHECK(board::move_object(scene, body_id, {1.0f, 0.0f}) && near(scene.boxes[1].center[0], -7.0f));
        CHECK(!board::move_object(scene, 999, {1.0f, 0.0f}));

        // Removing takes its instances and clears the selection.
        CHECK(board::remove_object(scene, chip));
        CHECK(scene.meshes.empty() && scene.find(chip) == nullptr && scene.selected == 0);
        CHECK(!board::remove_object(scene, chip));

        // remove_model() takes the objects its instances made.
        const board::object_id again = board::add_model(scene, meshes, math::mat4f::identity());
        CHECK(scene.find(again) != nullptr && scene.find(again)->kind == board::object_kind::part_model);
        board::remove_model(scene, meshes);
        CHECK(scene.find(again) == nullptr && scene.meshes.empty() && scene.objects.size() == 3);

        // Ids are not used twice.
        const board::object_id later = scene.add_object({});
        CHECK(later > again);
    }

    // Picking by shape, not by bounds: a turned box, and a model whose
    // triangle fills only half of its bounds, under a transform that turns
    // and scales it.
    void exact_picking()
    {
        board::scene scene;
        scene.boxes.push_back({{0.0f, 0.0f, -0.8f}, {80.0f, 50.0f, 1.6f}, 0.0f, {0, 1, 0}});
        // 10 long, 2 wide, along the diagonal y = x.
        scene.boxes.push_back({{-20.0f, 0.0f, 1.0f}, {10.0f, 2.0f, 2.0f}, std::numbers::pi_v<float> / 4.0f,
                               {0, 0, 0}, 0, "Q1"});
        CHECK(board::adopt_boxes(scene) == 2);
        const board::object_id board_id = scene.boxes[0].object;
        const board::object_id turned = scene.boxes[1].object;

        auto shape = std::make_shared<board::pick_shape>();
        shape->positions = {{0.0f, 0.0f, 0.0f}, {4.0f, 0.0f, 0.0f}, {0.0f, 4.0f, 0.0f}};
        shape->indices = {0, 1, 2};
        board::object o;
        o.kind = board::object_kind::dropped_model;
        o.name = "wedge";
        // Twice as big, turned a quarter, its corner at (10, 10, 1): the
        // triangle (10, 10), (10, 18), (2, 10).
        const math::mat4f placement = math::affine(
            math::rotation_z(std::numbers::pi_v<float> / 2.0f) * math::scaling(math::vec3f{2.0f, 2.0f, 2.0f}),
            math::vec3f{10.0f, 10.0f, 1.0f});
        model::bounds local;
        local.add(math::vec3f{0.0f, 0.0f, 0.0f});
        local.add(math::vec3f{4.0f, 4.0f, 0.0f});
        o.bounds = board::placed_bounds(local, placement);
        const board::object_id wedge = scene.add_object(std::move(o));
        scene.meshes.push_back({board::mesh_handle{}, placement, {1, 1, 1}, wedge, shape});

        const auto down = [](float x, float y) { return board::ray{{x, y, 50.0f}, {0.0f, 0.0f, -1.0f}}; };

        // On the turned box, and inside its bounds but off it.
        CHECK(board::pick(scene, down(-17.0f, 3.0f)) == turned);
        CHECK(board::pick(scene, down(-16.5f, -3.5f)) == board_id);
        const auto on_box = board::hit(down(-20.0f, 0.0f), scene.boxes[1]);
        CHECK(on_box.has_value() && near(*on_box, 48.0f));

        // On the triangle, and in the empty half of its bounds.
        CHECK(board::pick(scene, down(9.0f, 11.0f)) == wedge);
        CHECK(board::pick(scene, down(3.0f, 17.0f)) == board_id);
        const auto on_wedge = board::hit(down(9.0f, 11.0f), scene.meshes[0]);
        CHECK(on_wedge.has_value() && near(*on_wedge, 49.0f));

        // From below, the other side of the same triangle.
        CHECK(board::hit(board::ray{{9.0f, 11.0f, -50.0f}, {0.0f, 0.0f, 1.0f}}, scene.meshes[0]).has_value());

        // Without a shape, the bounds decide, as before.
        scene.meshes[0].shape.reset();
        CHECK(board::pick(scene, down(3.0f, 17.0f)) == wedge);
    }

    void marks()
    {
        model::bounds b;
        b.min = {0.0f, 0.0f, 0.0f};
        b.max = {4.0f, 2.0f, 1.0f};
        std::vector<board::box> m;
        board::outline(m, b, 0.1f, {1, 0, 0});
        CHECK(m.size() == 12);

        // Every edge lies along the box: the union of the bars' bounds is the
        // box, grown by half their width.
        model::bounds all;
        for (const board::box &bar : m)
            all.add(board::bounds_of(bar));
        CHECK(near(all.min[0], -0.05f) && near(all.max[0], 4.05f) && near(all.max[2], 1.05f));
        board::outline(m, model::bounds{}, 0.1f, {1, 0, 0});
        CHECK(m.size() == 12);

        // A measurement: a bar from point to point.
        std::vector<board::box> bars;
        board::bar(bars, {0.0f, 0.0f}, {3.0f, 4.0f}, 0.1f, 0.2f, {1, 1, 1});
        CHECK(bars.size() == 1 && near(bars[0].size[0], 5.0f) && near(bars[0].center[0], 1.5f) &&
              near(bars[0].rotation, std::atan2(4.0f, 3.0f)));
    }

    // ---- the view controller ---------------------------------------------------

    struct recorder
    {
        int clicks = 0;
        math::vec2f clicked_at{};
        std::vector<math::vec2f> grabs;
        std::optional<bool> released;
        bool grabbable = false;

        void attach(ui::board_view_controller &c)
        {
            c.can_grab = [this](const math::vec2f &, const math::vec2f &) { return grabbable; };
            c.clicked = [this](const math::vec2f &at, const math::vec2f &, ui::modifiers) {
                ++clicks;
                clicked_at = at;
            };
            c.grabbed = [this](const math::vec2f &, const math::vec2f &to, const math::vec2f &) { grabs.push_back(to); };
            c.released = [this](bool cancelled) { released = cancelled; };
        }
    };

    ui::viewport_pointer at(float x, float y, ui::mouse_buttons held = ui::mouse_buttons::none,
                            ui::modifiers mods = ui::modifiers::none)
    {
        ui::viewport_pointer e;
        e.position = {x, y};
        e.size = {800.0f, 600.0f};
        e.buttons = held;
        e.mods = mods;
        return e;
    }

    // A left press at (x0, y0), moves to (x1, y1) in steps, release.
    void drag(ui::board_view_controller &c, float x0, float y0, float x1, float y1,
              ui::modifiers mods = ui::modifiers::none)
    {
        (void)c.pointer_down(at(x0, y0, ui::mask_of(ui::mouse_button::left), mods));
        math::vec2f last{x0, y0};
        for (int i = 1; i <= 4; ++i)
        {
            ui::viewport_pointer e = at(x0 + (x1 - x0) * static_cast<float>(i) / 4.0f,
                                        y0 + (y1 - y0) * static_cast<float>(i) / 4.0f, ui::mask_of(ui::mouse_button::left), mods);
            e.delta = e.position - last;
            last = e.position;
            (void)c.pointer_move(e);
        }
        (void)c.pointer_up(at(x1, y1, ui::mouse_buttons::none, mods));
    }

    void controller()
    {
        ui::orbit_controller orbit;
        ui::board_view_controller c(orbit);
        recorder r;
        r.attach(c);
        const float yaw = orbit.yaw;

        // A press and release that hardly moves is a click; the camera stays.
        drag(c, 100.0f, 100.0f, 102.0f, 101.0f);
        CHECK(r.clicks == 1 && r.clicked_at == math::vec2f{100.0f, 100.0f});
        CHECK(orbit.yaw == yaw);

        // Further, it orbits, by the whole way moved, and is no click.
        drag(c, 100.0f, 100.0f, 200.0f, 100.0f);
        CHECK(r.clicks == 1);
        CHECK(near(orbit.yaw, yaw - 100.0f * orbit.orbit_speed));

        // Over something grabbable it grabs instead; the camera stays.
        r.grabbable = true;
        const float before = orbit.yaw;
        drag(c, 100.0f, 100.0f, 200.0f, 150.0f);
        CHECK(!r.grabs.empty() && r.grabs.back() == math::vec2f{200.0f, 150.0f});
        CHECK(r.released.has_value() && !*r.released);
        CHECK(orbit.yaw == before && r.clicks == 1);

        // Escape cancels a grab; what is left of the drag does nothing.
        r.grabs.clear();
        r.released.reset();
        (void)c.pointer_down(at(10.0f, 10.0f, ui::mask_of(ui::mouse_button::left)));
        (void)c.pointer_move(at(40.0f, 10.0f, ui::mask_of(ui::mouse_button::left)));
        CHECK(c.grabbing() && r.grabs.size() == 1);
        ui::key_event escape{};
        escape.key = ui::key::escape;
        escape.action = ui::action::press;
        CHECK(c.key(escape, {800.0f, 600.0f}));
        CHECK(r.released.has_value() && *r.released && !c.grabbing());
        (void)c.pointer_move(at(80.0f, 10.0f, ui::mask_of(ui::mouse_button::left)));
        (void)c.pointer_up(at(80.0f, 10.0f));
        CHECK(r.grabs.size() == 1 && r.clicks == 1);

        // Shift never grabs: it pans, as the orbit controller does.
        const math::vec3f target = orbit.target;
        drag(c, 100.0f, 100.0f, 200.0f, 100.0f, ui::modifiers::shift);
        CHECK(r.grabs.size() == 1);
        CHECK(!(orbit.target == target));

        // The wheel dollies whatever.
        const float distance = orbit.distance;
        CHECK(c.scroll({0.0f, 1.0f}, at(0.0f, 0.0f)));
        CHECK(orbit.distance < distance);
    }
} // namespace

int main()
{
    rays_undo_the_projection();
    hits();
    adopting_and_picking();
    exact_picking();
    marks();
    controller();
    return volt::tests::result("app board");
}
