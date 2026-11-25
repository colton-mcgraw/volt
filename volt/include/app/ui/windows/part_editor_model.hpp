#pragma once

#include <history/record.hpp>
#include <history/stack.hpp>
#include <library/library.hpp>
#include <library/part.hpp>
#include <library/schema.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// -------------------------------------------------------------------------
// part editor's model
// -------------------------------------------------------------------------
//
// What the part editor (<app/ui/windows/part_editor.hpp>) edits, worked out
// apart from any widget, so that it can be tested without a window:
//
//   the part           its fields as text, its parameters in order; whether
//                      it differs from what was opened or last saved
//                      (modified())
//   undo and redo      every edit a step of the history
//                      (<history/stack.hpp>), labelled for the menu
//                      ("Edit name", "Move parameter"); see below
//   the checks         the schema's (<library/schema.hpp>) issues with the
//                      part, run again after every change, and what stops
//                      it being saved
//   the schema's help  parameter names completed from the schema's names
//                      and aliases while typed, a misspelling's probable
//                      meaning, a device's parameters added with their units
//   the id             the part's identity, which nothing here changes: a
//                      part being edited keeps its own; a new part and a
//                      duplicate have none until commit() puts them into a
//                      library, which gives them one (library::generate_id,
//                      <library/edit.hpp>)
//   commit()           puts the part into a library through
//                      <library/edit.hpp>: added, or put in the place of the
//                      one opened, with the 3D model file it names
//
// Three ways to start:
//
//   start_new()          an empty part, for a library chosen later
//   start_edit(p)        a part of a library, changed in place: its id there
//                        is original_id(), and saving replaces that part
//   start_duplicate(p)   a copy of a part of another library, to be added as
//                        a new part, under an id of its own, with its origin
//                        recorded (as a project does, <library/project.hpp>)
//                        and its model file carried along when its library
//                        holds it
//
// Undo. What an edit changes is the part and the model file attached to
// it, together: the history's document, small enough to be kept whole
// before and after each step (history::value_change), the file's bytes
// shared rather than copied. The mode, the original id and the files a
// duplicate carries are where the part is saved, not edits, and undo
// leaves them. Typing into one field (or one parameter's name, value or
// unit) is one step, however many keystrokes: it ends when another field
// or another kind of edit is made, when the step is undone, when the part
// is saved, and when seal() is called (the window does when a field loses
// the focus). An edit that changes nothing is no step, and typing that ends
// where it began leaves none. start_*() begins a new history; commit()
// keeps it, so that undo goes back past a save, and modified() still says
// whether the part differs from the one saved.
//
// The model does not read or write library files, except a 3D model file
// the user names: whoever saves loads the target library, calls commit()
// and saves it (library::save_library). Interface thread; nothing locks.

namespace volt::ui
{

    // The part's text fields, in the order the editor shows them.
    enum class part_field : std::uint8_t
    {
        name,
        id,
        category,
        description,
        manufacturer,
        mpn,
        value,
        package,
        prefix,
        device,
        symbol,
        footprint,
        model,
        datasheet,
        keywords,
    };

    inline constexpr std::size_t part_field_count = 15;

    // "Name", "ID", "MPN", ...: what the editor calls the field.
    std::string_view field_title(part_field field) noexcept;

    // An example of what goes in the field, for its placeholder.
    std::string_view field_placeholder(part_field field) noexcept;

    // Keywords as typed in one field: separated by commas, each trimmed,
    // empty ones dropped. "resistor, chip,, smd " is {"resistor", "chip",
    // "smd"}; join_keywords() writes them back as "resistor, chip, smd".
    std::vector<std::string> parse_keywords(std::string_view text);
    std::string join_keywords(std::span<const std::string> keywords);

    // Whether a library file is one the application ships: below the assets
    // directory. Those are replaced by the next version of Volt and are not
    // changed in place; their parts are duplicated into a user library
    // instead. Compared as absolute, normalized paths.
    bool is_shipped_library(const std::filesystem::path &file, const std::filesystem::path &assets);

    // A library file whose parts may be changed in place: one that has a
    // file, and is not shipped.
    bool is_writable_library(const std::filesystem::path &file, const std::filesystem::path &assets);

    // Removes the library's embedded file `name` when no part names it any
    // more: the one a part named before it was given another model, or
    // before it was deleted. True when it did. The library's other unused
    // files are none of that change's business, and stay
    // (library::remove_unused_files() takes them all).
    bool remove_file_if_unused(library::library &lib, std::string_view name);

    // A parameter the schema knows, offered for a name being typed.
    struct parameter_completion
    {
        std::string name; // the definition's name: "Resistance"
        std::string unit; // its unit: "Ω"; empty for none
        // The alias that matched, when it was not the name: "Ohms".
        std::string alias;

        bool operator==(const parameter_completion &) const = default;
    };

    // A device the part may be: id as part::device holds it, name for
    // people. The first choice is {"", "None"}.
    struct device_choice
    {
        std::string id;
        std::string name;

        bool operator==(const device_choice &) const = default;
    };

    enum class editor_mode : std::uint8_t
    {
        create,    // a new part
        edit,      // a part of the target library, saved in its place
        duplicate, // a copy of a part of another library, added as a new part
    };

    class part_editor_model
    {
    public:
        // A model copies, history and all, the copy's its own from then on:
        // the window saves through a copy, so that a failed write leaves the
        // editor as it was.
        explicit part_editor_model(const library::schema &schema = library::schema::builtin());

        // ---- what is edited ----------------------------------------------------------

        void start_new();
        void start_edit(const library::part &part);
        // `source` is the library the part is in: its name and version go
        // into the copy's origin, and the model file the part names comes
        // along when the library carries it.
        void start_duplicate(const library::part &part, const library::library &source);

        editor_mode mode() const noexcept { return m_mode; }
        const library::part &part() const noexcept { return m_doc.part; }

        // The id the part has in the target library while editing it in
        // place; empty for a new part or a duplicate, which are given one
        // when committed.
        const std::string &original_id() const noexcept { return m_original_id; }

        // The part (or its model file) differs from what start_*() or the
        // last commit() left: compared, so that undoing back to that state
        // is unmodified again, whichever way it was reached.
        bool modified() const;

        // ---- undo ------------------------------------------------------------------------

        bool can_undo() const noexcept { return m_history.can_undo(); }
        bool can_redo() const noexcept { return m_history.can_redo(); }

        // What undo() and redo() would take back or make again, for the
        // menu ("Undo Edit name"); empty when nothing. Valid until the next
        // change.
        std::string_view undo_label() const noexcept { return m_history.undo_label(); }
        std::string_view redo_label() const noexcept { return m_history.redo_label(); }

        // False when there is nothing to undo or redo.
        bool undo();
        bool redo();

        // Ends the step being typed: what is typed next into the same field
        // is a step of its own. For when the field loses the focus.
        void seal() noexcept { m_history.seal(); }

        // Changes (only ever grows) whenever the part, the checks, modified()
        // or what undo and redo would do may have: after every edit, undo,
        // redo, start and commit. A window compares it with the one it last
        // showed. The history's: every change to the part goes through it.
        std::uint64_t revision() const noexcept { return m_history.revision(); }

        // ---- fields --------------------------------------------------------------------

        // The field as the editor shows it (keywords joined with ", ").
        // The id's is empty until the part has one.
        std::string field(part_field f) const;

        // Sets a field from what was typed. The id is not typed: setting it
        // does nothing.
        void set_field(part_field f, std::string_view text);

        // ---- parameters ------------------------------------------------------------------

        std::size_t parameter_count() const noexcept { return m_doc.part.parameters.size(); }
        const library::parameter &parameter(std::size_t index) const { return m_doc.part.parameters[index]; }

        // Adds a parameter at the end and returns its index.
        std::size_t add_parameter(library::parameter p = {});
        // Adds the schema's parameter of that name (or alias) with its
        // definition's name and unit and no value; one the part has already
        // is not added again. Returns its index, the existing one's if so.
        std::size_t add_known_parameter(std::string_view name);
        void remove_parameter(std::size_t index);
        // Moves a parameter to another position; false if either is out of
        // range or they are the same.
        bool move_parameter(std::size_t from, std::size_t to);

        void set_parameter_name(std::size_t index, std::string_view name);
        void set_parameter_value(std::size_t index, std::string_view value);
        void set_parameter_unit(std::size_t index, std::string_view unit);

        // The schema's parameters whose name or an alias starts with what
        // is typed, compared loosely (case, spaces, '_', '-' and '.' do not
        // count): "res" offers Resistance, Reverse voltage...; "ohm" offers
        // Resistance by its alias Ohms. Names before aliases, then schema
        // order. Nothing for empty text.
        std::vector<parameter_completion> complete_parameter(std::string_view typed, std::size_t limit = 6) const;

        // Every parameter the schema knows, in its order.
        std::vector<parameter_completion> known_parameters() const;

        // What the name of parameter `index` probably should be, best
        // first: nothing when it is a known name as the schema writes it;
        // the definition's name when it is an alias or written otherwise
        // ("ohms", "forward_voltage"); else the names it starts, else the
        // names it is close to (a misspelling).
        std::vector<parameter_completion> parameter_hints(std::size_t index, std::size_t limit = 3) const;

        // Renames parameter `index` to the completion, and gives it the
        // completion's unit when it has none.
        void apply_completion(std::size_t index, const parameter_completion &completion);

        // The schema's issues about parameter `index` (by its name).
        std::vector<library::issue> parameter_issues(std::size_t index) const;

        // ---- the device ------------------------------------------------------------------

        // {"", "None"}, then every device of the schema, and the part's own
        // device last when the schema does not know it.
        std::vector<device_choice> device_choices() const;

        // The position of the part's device among device_choices(), found
        // through the schema's names and aliases ("transistor" is the
        // bipolar transistor's); 0, None, for no device.
        std::size_t device_index() const;

        // The parameters the part's device has that the part lacks, as the
        // schema names them; the required ones only, or all.
        std::vector<library::device_parameter> missing_parameters(bool required_only = true) const;

        // Adds those, with their definitions' units and no value; returns
        // how many.
        std::size_t add_device_parameters(bool required_only = true);

        // ---- checks ----------------------------------------------------------------------

        // The schema's check of the part as it is now. Errors here do not
        // stop a save: they are what a simulation of the part would stumble
        // on, and a part is often saved before it is finished.
        const std::vector<library::issue> &issues() const noexcept { return m_issues; }

        // What stops the part being saved, one sentence each: neither a
        // name nor an MPN, a parameter with a value and no name, a model
        // file that is not there. Empty when it can be saved.
        std::vector<std::string> save_problems() const;
        bool can_save() const { return save_problems().empty(); }

        // ---- the 3D model ----------------------------------------------------------------

        // A model file the user gave (dropped on the window, or typed as an
        // absolute path): read now, put into the target library on commit()
        // (library::add_file), and named by the part from then on. The
        // model field shows the file's name.
        std::expected<void, std::string> attach_model(const std::filesystem::path &file);
        const std::optional<library::embedded_file> &attached_model() const noexcept;

        // ---- saving ----------------------------------------------------------------------

        // Puts the part into `target`: added for a new part or a duplicate,
        // under an id `target` gives it (library::add_part), in the place of
        // original_id() when editing (library::replace_part). Parameters
        // with no name, value or unit are left out; a model file goes in
        // with it, and the file of the model it had goes out when nothing
        // else names it (remove_file_if_unused()). Returns the part's id
        // there. Afterwards the model edits that part in place, as the
        // library has it, unmodified; the history stays, every state in it
        // made over as the library made the part (history::stack::rewrite).
        // On failure `target` and the model are as they were.
        std::expected<std::string, std::string> commit(library::library &target);

    private:
        // What an edit changes, and undo puts back. The model file is
        // shared: a step keeps the document before and after it, and a 3D
        // model's bytes (megabytes, for a STEP file) are not to be copied
        // into every step. Null for none; never an empty optional.
        struct document
        {
            library::part part;
            std::shared_ptr<const std::optional<library::embedded_file>> model_file;

            bool operator==(const document &) const = default;
        };

        using change = history::value_change<document>;
        using history_stack = history::stack<document, change>;

        void start(editor_mode mode, library::part part);
        void changed();
        // Makes `next` the document, as a step labelled `label`; nothing
        // when it is the document already. Successive edits with the same
        // non-empty `merge_key` are one step (typing into one field).
        void edit(std::string label, document next, std::string merge_key = {});
        // Adds the schema's parameter `name` to `p` unless it has it;
        // returns its index (add_known_parameter()).
        std::size_t add_known(library::part &p, std::string_view name) const;
        // The part as it would be saved: parameters trimmed, blank ones
        // and (for the checks) nameless ones left out.
        library::part cleaned() const;

        const library::schema *m_schema;
        editor_mode m_mode = editor_mode::create;
        document m_doc;
        // The document as start_*() or the last commit() left it, for
        // modified(), which compares rather than asks the history's saved
        // marker: edits that end where the part was saved, however many
        // steps they took, leave it unmodified.
        document m_baseline;
        std::string m_original_id;

        // For a duplicate: the source library's file the part names, if it
        // carries it, to go along (library::carry_files).
        std::vector<library::embedded_file> m_carried;

        std::vector<library::issue> m_issues;

        history_stack m_history;
    };

} // namespace volt::ui
