#pragma once

#include <app/board/model.hpp>
#include <app/board/scene.hpp>
#include <app/ui/schematic_document.hpp>
#include <model/model.hpp>
#include <model/options.hpp>
#include <ui/signal.hpp>
#include <ui/theme.hpp>

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// -------------------------------------------------------------------------
// documents
// -------------------------------------------------------------------------
//
// What the windows show and edit, shared by all of them: the schematic and
// the board, each as the scene its renderer draws plus the state that goes
// with it. They live in app_services (<app/ui/services.hpp>), as long as
// the application, whether or not a window shows them; so a window that is
// closed and opened again finds them as they were, and a file dropped on
// one window can change what another shows.
//
// The schematic is the real document: the open sheets, each with its undo
// history, saved and loaded with the project, and simulated from its
// netlist (<app/ui/schematic_document.hpp>). The board is still the demo
// board of <app/demo.hpp>, with the models the user drops on it.
//
// Interface thread only, the render loop included (it reads the scenes and
// the dirty flags between laying out and painting).
//
// Dirty flags. A view is rendered into its offscreen target only when its
// scene changed (mark_dirty(), take_dirty()), its viewport changed size, or
// (the schematic) the simulation has new values. Whoever changes a scene
// marks it dirty and invalidates the viewport showing it, so that the window
// asks for a frame; `changed` says so to anyone else who cares.

namespace volt::ui
{

    // The board: its scene, and what is on it as objects (<app/board/
    // scene.hpp>): the board itself, the parts and their models, and the
    // models the user dropped on it. The demo board's objects are fixed;
    // dropped models can be moved in the board's plane and taken off again.
    // Any object can be hidden, and so can a kind of object; one can be
    // selected, which the view shows tinted.
    class board_document
    {
    public:
        // A model the user put on the board.
        struct placed_model
        {
            std::string file;
            board::model_meshes meshes;

            // Its object in the scene; where it is lives there.
            board::object_id object = 0;
        };

        // What the board view draws, camera included.
        board::scene scene;

        // Where meshes are made. Set once by the application.
        void set_renderer(board::renderer *renderer) noexcept { m_renderer = renderer; }

        void mark_dirty()
        {
            m_dirty = true;
            changed.emit();
        }
        bool take_dirty() noexcept { return std::exchange(m_dirty, false); }

        // Puts a prepared model on the board: at `where` (a point on the
        // board's top, millimeters) or, without one, a free corner of the
        // demo board. `centre` stands it on the board centred on the spot
        // (OBJ, STL); otherwise its own origin goes there (STEP models from
        // a part library carry the footprint's). Returns what to tell the
        // user, or why it could not be placed. The new model is selected.
        // Emits model_placed with where it ended up, then models_changed.
        std::expected<std::string, std::string> place(std::string file, const board::prepared_model &prepared,
                                                      const model::load_report &report,
                                                      std::optional<math::vec2f> where, bool centre);

        // Takes every placed model off the board again; how many there were.
        std::size_t remove_models();

        const std::vector<placed_model> &models() const noexcept { return m_models; }

        // ---- objects --------------------------------------------------------------

        const std::vector<board::object> &objects() const noexcept { return scene.objects; }
        const board::object *find(board::object_id id) const noexcept { return scene.find(id); }

        // Gives the scene's boxes that have none an object (board::
        // adopt_boxes()): the demo board's body and part bodies.
        void adopt_boxes();

        board::object_id selected() const noexcept { return scene.selected; }

        // Selects `id` (0: nothing). Emits selection_changed if that changes
        // anything.
        void select(board::object_id id);

        // The user's switch of one object, and of a whole kind.
        void set_visible(board::object_id id, bool visible);
        bool shown(board::object_kind kind) const noexcept { return scene.shown[static_cast<std::size_t>(kind)]; }
        void set_shown(board::object_kind kind, bool shown);

        // Whether the object is a dropped model: those can be moved and
        // taken off; the demo board's own objects cannot.
        bool is_dropped(board::object_id id) const noexcept;

        // Takes a dropped model off the board. False for anything else.
        bool remove(board::object_id id);

        // Moves a dropped model by `offset` in the board's plane. False for
        // anything else.
        bool move(board::object_id id, const math::vec2f &offset);

        // ---- signals ---------------------------------------------------------------

        // After every change of the scene (mark_dirty()).
        signal<> changed;

        // A model was placed, with its bounds on the board: for the board
        // view to frame it.
        signal<const model::bounds &> model_placed;

        // Models were placed or removed: for lists of them.
        signal<> models_changed;

        // Objects came, went, moved or were shown or hidden.
        signal<> objects_changed;

        // Another object (or none) is selected.
        signal<> selection_changed;

    private:
        board::renderer *m_renderer = nullptr;
        std::vector<placed_model> m_models;
        bool m_dirty = true;
    };

} // namespace volt::ui
