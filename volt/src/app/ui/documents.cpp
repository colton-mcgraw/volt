#include <app/ui/documents.hpp>

#include <app/board/renderer.hpp>

#include <math/transform.hpp>

#include <algorithm>
#include <format>

namespace volt::ui
{
    namespace
    {
        // Where a model goes that was not dropped on the board view: a
        // corner of the demo board that nothing else uses.
        constexpr math::vec2f free_spot{-28.0f, -12.0f};
    } // namespace

    // ---- board --------------------------------------------------------------------

    std::expected<std::string, std::string> board_document::place(std::string file,
                                                                  const board::prepared_model &prepared,
                                                                  const model::load_report &report,
                                                                  std::optional<math::vec2f> where, bool centre)
    {
        if (m_renderer == nullptr)
            return std::unexpected("the board has no renderer");

        std::string skipped;
        if (report.faces_failed > 0)
            skipped += std::format(", {} face{} skipped", report.faces_failed, report.faces_failed == 1 ? "" : "s");
        std::size_t unsupported = 0;
        for (const auto &[type, count] : report.unsupported)
            unsupported += count;
        if (unsupported > 0)
            skipped += std::format(", {} unsupported entit{}", unsupported, unsupported == 1 ? "y" : "ies");

        if (prepared.parts.empty() || prepared.instances.empty())
            return std::unexpected(file + " has nothing to show" + skipped);

        auto meshes = board::upload(*m_renderer, prepared);
        if (!meshes)
            return std::unexpected("Cannot show " + file + ": " + meshes.error());

        const math::vec2f at = where.value_or(free_spot);
        math::vec3f offset{at.x(), at.y(), 0.0f};
        if (centre)
        {
            const math::vec3f middle = meshes->bounds.center();
            offset -= math::vec3f{middle[0], middle[1], meshes->bounds.min[2]};
        }
        const math::mat4f placement = math::translation(offset);
        const board::object_id id = board::add_model(scene, *meshes, placement, board::object_kind::dropped_model, file);
        const model::bounds placed = board::placed_bounds(meshes->bounds, placement);

        std::string text = std::format("Loaded {}: {} triangles in {:.2f} s{}", file, prepared.triangles,
                                       report.parse_seconds + report.build_seconds, skipped);

        m_models.push_back({std::move(file), std::move(*meshes), id});
        scene.selected = id;
        mark_dirty();
        model_placed.emit(placed);
        models_changed.emit();
        objects_changed.emit();
        selection_changed.emit();
        return text;
    }

    std::size_t board_document::remove_models()
    {
        if (m_models.empty() || m_renderer == nullptr)
            return 0;

        const board::object_id was_selected = scene.selected;
        for (placed_model &placed : m_models)
        {
            board::remove_object(scene, placed.object);
            board::release(*m_renderer, placed.meshes);
        }
        const std::size_t count = m_models.size();
        m_models.clear();
        mark_dirty();
        models_changed.emit();
        objects_changed.emit();
        if (scene.selected != was_selected)
            selection_changed.emit();
        return count;
    }

    // ---- board objects -------------------------------------------------------------

    void board_document::adopt_boxes()
    {
        if (board::adopt_boxes(scene) == 0)
            return;
        mark_dirty();
        objects_changed.emit();
    }

    void board_document::select(board::object_id id)
    {
        if (id != 0 && scene.find(id) == nullptr)
            id = 0;
        if (id == scene.selected)
            return;
        scene.selected = id;
        mark_dirty();
        selection_changed.emit();
    }

    void board_document::set_visible(board::object_id id, bool visible)
    {
        board::object *o = scene.find(id);
        if (o == nullptr || o->visible == visible)
            return;
        o->visible = visible;
        mark_dirty();
        objects_changed.emit();
    }

    void board_document::set_shown(board::object_kind kind, bool shown)
    {
        bool &flag = scene.shown[static_cast<std::size_t>(kind)];
        if (flag == shown)
            return;
        flag = shown;
        mark_dirty();
        objects_changed.emit();
    }

    bool board_document::is_dropped(board::object_id id) const noexcept
    {
        return id != 0 && std::ranges::find(m_models, id, &placed_model::object) != m_models.end();
    }

    bool board_document::remove(board::object_id id)
    {
        const auto found = std::ranges::find(m_models, id, &placed_model::object);
        if (id == 0 || found == m_models.end() || m_renderer == nullptr)
            return false;

        const bool was_selected = scene.selected == id;
        board::remove_object(scene, id);
        board::release(*m_renderer, found->meshes);
        m_models.erase(found);
        mark_dirty();
        models_changed.emit();
        objects_changed.emit();
        if (was_selected)
            selection_changed.emit();
        return true;
    }

    bool board_document::move(board::object_id id, const math::vec2f &offset)
    {
        if (!is_dropped(id) || !board::move_object(scene, id, offset))
            return false;
        mark_dirty();
        objects_changed.emit();
        return true;
    }

} // namespace volt::ui
