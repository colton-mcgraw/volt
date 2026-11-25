#include "math/check.hpp"

#include <app/board/model.hpp>
#include <app/ui/image_registry.hpp>

#include <math/transform.hpp>

#include <cmath>

// The application's pieces that need no GPU: the index space of the
// interface's images, and how a loaded model becomes the board's meshes and
// instances (<app/board/model.hpp>).

using namespace volt;

namespace
{
    void registry()
    {
        ui::image_registry r;
        const ui::texture_id a = r.allocate();
        const ui::texture_id b = r.allocate();
        const ui::texture_id c = r.allocate();
        CHECK(a == ui::texture_id::image(0) && b == ui::texture_id::image(1) && c == ui::texture_id::image(2));
        CHECK(r.size() == 3 && r.capacity() == 3);

        // The lowest free index goes out next; the table does not grow.
        r.release(b);
        CHECK(!r.contains(b) && r.contains(a) && r.contains(c));
        CHECK(r.allocate() == ui::texture_id::image(1));
        CHECK(r.capacity() == 3);

        // Giving back twice, or what was never handed out, changes nothing.
        r.release(c);
        r.release(c);
        r.release(ui::texture_id::image(40));
        r.release(ui::texture_id::glyph_page(0));
        CHECK(r.size() == 2);
        CHECK(!r.contains(ui::texture_id::glyph_page(0)));
    }

    model::mesh triangle_pair(bool coloured)
    {
        // Two triangles side by side in z = 0, each with its own vertices.
        model::mesh mesh;
        mesh.name = "pair";
        const math::vec3f up{0.0f, 0.0f, 1.0f};
        mesh.vertices = {{{0, 0, 0}, up}, {{1, 0, 0}, up}, {{0, 1, 0}, up},
                         {{2, 0, 0}, up}, {{3, 0, 0}, up}, {{2, 1, 0}, up}};
        mesh.indices = {0, 1, 2, 3, 4, 5};
        mesh.material.color = {0.5f, 0.5f, 0.5f};
        if (coloured)
        {
            const math::vec3f red{1, 0, 0};
            const math::vec3f blue{0, 0, 1};
            mesh.colors = {red, red, red, blue, blue, blue};
        }
        return mesh;
    }

    void prepare_plain()
    {
        model::model m;
        m.name = "plain";
        m.meshes.push_back(triangle_pair(false));
        model::node root;
        root.meshes = {0};
        m.nodes.push_back(root);

        const board::prepared_model p = board::prepare(m);
        CHECK(p.name == "plain");
        CHECK(p.parts.size() == 1);
        CHECK(p.parts[0].vertices.size() == 6 && p.parts[0].indices.size() == 6);
        CHECK(p.parts[0].color == math::vec3f{0.5f, 0.5f, 0.5f});
        CHECK(p.instances.size() == 1 && p.triangles == 2);
        CHECK(p.bounds.min == math::vec3f{0, 0, 0} && p.bounds.max == math::vec3f{3, 1, 0});
    }

    void prepare_coloured_and_placed()
    {
        // A mesh with two colours, placed twice: once at the root, once in a
        // child moved along y.
        model::model m;
        m.meshes.push_back(triangle_pair(true));
        model::node root;
        root.meshes = {0};
        root.children = {1};
        model::node child;
        child.meshes = {0};
        child.transform = math::translation(math::vec3f{0.0f, 10.0f, 0.0f});
        m.nodes = {root, child};

        const board::prepared_model p = board::prepare(m);
        CHECK(p.parts.size() == 2);
        if (p.parts.size() == 2)
        {
            CHECK(p.parts[0].color == math::vec3f{1, 0, 0} && p.parts[1].color == math::vec3f{0, 0, 1});
            // Only the vertices each colour uses, renumbered.
            CHECK(p.parts[0].vertices.size() == 3 && p.parts[1].vertices.size() == 3);
            CHECK(p.parts[1].indices == std::vector<std::uint32_t>({0, 1, 2}));
            CHECK(p.parts[1].vertices[0].position == math::vec3f{2, 0, 0});
        }
        CHECK(p.instances.size() == 4 && p.triangles == 4);
        CHECK(p.bounds.max == math::vec3f{3, 11, 0});

        // On the board: every instance under the placement, by the handles
        // the upload made.
        board::model_meshes meshes;
        meshes.parts = {{0, 1}, {1, 1}};
        meshes.colors = {p.parts[0].color, p.parts[1].color};
        meshes.instances = p.instances;
        meshes.bounds = p.bounds;

        board::scene scene;
        scene.meshes.push_back({{7, 1}, math::mat4f::identity(), {1, 1, 1}}); // someone else's
        const math::mat4f placement = math::translation(math::vec3f{100.0f, 0.0f, 0.0f});
        board::add_model(scene, meshes, placement);
        CHECK(scene.meshes.size() == 5);
        if (scene.meshes.size() == 5)
        {
            const board::mesh_instance &moved = scene.meshes.back();
            CHECK(moved.transform(0, 3) == 100.0f && moved.transform(1, 3) == 10.0f);
            CHECK(moved.color == math::vec3f{0, 0, 1} && moved.mesh == board::mesh_handle{1, 1});
        }

        const model::bounds placed = board::placed_bounds(meshes.bounds, placement);
        CHECK(placed.min == math::vec3f{100, 0, 0} && placed.max == math::vec3f{103, 11, 0});

        board::remove_model(scene, meshes);
        CHECK(scene.meshes.size() == 1 && scene.meshes[0].mesh == board::mesh_handle{7, 1});
    }

    void prepare_broken()
    {
        // An index past the vertices drops its triangle; a mesh left empty
        // drops out; a node naming a mesh that is not there is skipped.
        model::model m;
        model::mesh bad = triangle_pair(false);
        bad.indices[4] = 99;
        m.meshes.push_back(bad);
        model::mesh empty;
        m.meshes.push_back(empty);
        model::node root;
        root.meshes = {0, 1, 5};
        m.nodes.push_back(root);

        const board::prepared_model p = board::prepare(m);
        CHECK(p.parts.size() == 1);
        CHECK(p.parts.size() == 1 && p.parts[0].indices.size() == 3);
        CHECK(p.instances.size() == 1 && p.triangles == 1);
    }

    void picking()
    {
        // A point on the board, projected as the renderer projects it, and
        // found again from the pixel it lands on.
        board::camera camera;
        camera.eye = {55.0f, -95.0f, 80.0f};
        camera.target = {0.0f, 0.0f, 0.0f};
        const math::vec2f size{640.0f, 400.0f};

        math::mat4f projection = math::perspective(camera.fov_y, size.x() / size.y(), camera.z_near, camera.z_far);
        for (std::size_t column = 0; column < 4; ++column)
            projection(1, column) = -projection(1, column);
        const math::mat4f view_projection = projection * math::look_at(camera.eye, camera.target, camera.up);

        for (const math::vec2f &spot : {math::vec2f{0.0f, 0.0f}, math::vec2f{-28.0f, -12.0f}, math::vec2f{31.0f, 14.0f}})
        {
            const math::vec4f clip = view_projection * math::vec4f{spot.x(), spot.y(), 0.0f, 1.0f};
            const math::vec2f pixel{(clip[0] / clip[3] + 1.0f) * 0.5f * size.x(),
                                    (clip[1] / clip[3] + 1.0f) * 0.5f * size.y()};
            const auto found = board::point_on_board(camera, pixel, size);
            CHECK(found.has_value());
            if (found)
                CHECK(std::abs(found->x() - spot.x()) < 0.01f && std::abs(found->y() - spot.y()) < 0.01f);
        }

        // Looking level, above the horizon there is no board, below it there is.
        board::camera level = camera;
        level.eye = {0.0f, -100.0f, 10.0f};
        level.target = {0.0f, 0.0f, 10.0f};
        CHECK(!board::point_on_board(level, {320.0f, 10.0f}, size).has_value());
        CHECK(board::point_on_board(level, {320.0f, 390.0f}, size).has_value());
    }
} // namespace

int main()
{
    registry();
    prepare_plain();
    prepare_coloured_and_placed();
    prepare_broken();
    picking();
    return volt::tests::result("app");
}
