#include "../math/check.hpp"

#include <app/ui/windows/part_editor_model.hpp>

#include <json/json.hpp>
#include <library/edit.hpp>
#include <library/project.hpp>
#include <library/schema.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// The part editor's model without a window: fields and keywords as typed,
// the id a part is given when first saved and keeps (not typed, not made
// from its name or MPN), the schema's checks run again on every change,
// parameter names completed and corrected from the schema, a device's
// parameters added with their units, and commit() adding, replacing and
// duplicating parts through <library/edit.hpp>, model files included (and
// the one a part no longer names taken out). Undo and redo of every kind of
// edit: typing one step per field, edits that change nothing (or typing
// that ends where it began) no step, the history begun again by start_*() and kept across commit(), with
// modified() right on both sides of a save. Also which library files are
// the shipped ones that are not changed in place.

using namespace volt;
using ui::editor_mode;
using ui::part_editor_model;
using ui::part_field;

namespace
{
    library::part make_part(std::string id, std::string name, std::string mpn = {})
    {
        library::part p;
        p.id = std::move(id);
        p.name = std::move(name);
        p.mpn = std::move(mpn);
        return p;
    }

    library::library my_parts()
    {
        library::library lib = library::new_library("my-parts", "My Parts");
        lib.parts.push_back(make_part("RC0603", "10k 0603", "RC0603"));
        lib.parts.push_back(make_part("LM358", "LM358 dual op amp", "LM358DR"));
        return lib;
    }

    bool has_issue(const std::vector<library::issue> &issues, library::issue_code code, std::string_view parameter = {})
    {
        return std::ranges::any_of(issues, [&](const library::issue &i) {
            return i.code == code && (parameter.empty() || i.parameter == parameter);
        });
    }

    // A directory of the test's own under the system's temporary one,
    // empty.
    std::filesystem::path scratch(std::string_view name)
    {
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / "volt-part-editor-tests" / name;
        std::error_code error;
        std::filesystem::remove_all(dir, error);
        std::filesystem::create_directories(dir, error);
        return dir;
    }

    void keywords_and_titles()
    {
        CHECK(ui::parse_keywords("resistor, chip,, smd ") == std::vector<std::string>{"resistor", "chip", "smd"});
        CHECK(ui::parse_keywords("").empty());
        CHECK(ui::parse_keywords(" , ,").empty());
        CHECK(ui::parse_keywords("one") == std::vector<std::string>{"one"});
        const std::vector<std::string> words{"a", "b c"};
        CHECK(ui::join_keywords(words) == "a, b c");
        CHECK(ui::field_title(part_field::mpn) == "MPN");
        CHECK(ui::field_title(part_field::model) == "3D model");
        for (std::size_t i = 0; i < ui::part_field_count; ++i)
            CHECK(!ui::field_title(static_cast<part_field>(i)).empty());

        part_editor_model m;
        m.set_field(part_field::keywords, "resistor,  smd,");
        CHECK(m.part().keywords == std::vector<std::string>{"resistor", "smd"});
        CHECK(m.field(part_field::keywords) == "resistor, smd");
        for (std::size_t i = 0; i < ui::part_field_count; ++i)
        {
            const auto f = static_cast<part_field>(i);
            if (f == part_field::id || f == part_field::keywords)
                continue;
            m.set_field(f, "x");
            CHECK(m.field(f) == "x");
        }
    }

    void ids()
    {
        // A new part has no id until it is saved, and the id is not typed.
        part_editor_model m;
        m.start_new();
        CHECK(m.mode() == editor_mode::create);
        CHECK(m.part().id.empty());
        CHECK(m.field(part_field::id).empty());
        CHECK(m.original_id().empty());
        CHECK(!m.modified());
        CHECK(!m.can_save()); // nothing to show it by in lists
        m.set_field(part_field::id, "typed");
        CHECK(m.part().id.empty());
        CHECK(!m.modified());

        // Neither the name nor the MPN makes one.
        m.set_field(part_field::name, "10k 0805");
        CHECK(m.can_save());
        m.set_field(part_field::mpn, "RC0603");
        CHECK(m.part().id.empty());
        CHECK(m.modified());

        // Given by the library it is saved into: one it has no part of, and
        // nothing like the MPN its other part has as its id.
        library::library target = my_parts();
        auto added = m.commit(target);
        CHECK(added.has_value());
        const std::string id = added.value_or("");
        CHECK(!id.empty() && id != "RC0603" && id != "RC0603-2");
        CHECK(target.parts.size() == 3);
        CHECK(target.find(id) != nullptr && target.find(id)->mpn == "RC0603");
        CHECK(m.mode() == editor_mode::edit);
        CHECK(m.part().id == id && m.original_id() == id && m.field(part_field::id) == id);

        // Kept, whatever else is corrected.
        m.set_field(part_field::mpn, "RC0805FR-0710KL");
        m.set_field(part_field::name, "10k 1% 0805");
        m.set_field(part_field::id, "renamed");
        CHECK(m.part().id == id);
        auto again = m.commit(target);
        CHECK(again && *again == id);
        CHECK(target.parts.size() == 3);
        CHECK(target.find("renamed") == nullptr);
        CHECK(target.find(id) != nullptr && target.find(id)->mpn == "RC0805FR-0710KL");

        // Two new parts of one MPN are two parts.
        part_editor_model twin;
        twin.set_field(part_field::mpn, "RC0805FR-0710KL");
        CHECK(twin.can_save());
        auto second = twin.commit(target);
        CHECK(second && *second != id);
        CHECK(target.parts.size() == 4);
    }

    void parameters()
    {
        part_editor_model m;
        m.set_field(part_field::name, "R");
        const std::size_t a = m.add_parameter({"Resistance", "10k", "\xCE\xA9", {}});
        const std::size_t b = m.add_parameter({"Tolerance", "1", "%", {}});
        const std::size_t blank = m.add_parameter();
        CHECK(a == 0 && b == 1 && blank == 2);
        CHECK(m.parameter_count() == 3);

        CHECK(m.move_parameter(1, 0));
        CHECK(m.parameter(0).name == "Tolerance");
        CHECK(m.parameter(1).name == "Resistance");
        CHECK(!m.move_parameter(0, 0));
        CHECK(!m.move_parameter(0, 7));

        // A blank row neither blocks a save nor is saved.
        CHECK(m.can_save());
        m.set_parameter_value(2, "3");
        CHECK(!m.can_save()); // a value with no name
        m.set_parameter_name(2, "Height");
        m.set_parameter_unit(2, "mm");
        CHECK(m.can_save());
        m.set_parameter_name(2, "");
        m.set_parameter_value(2, "");
        m.set_parameter_unit(2, "");
        CHECK(m.can_save());

        library::library target = library::new_library("mine");
        auto saved = m.commit(target);
        CHECK(saved.has_value());
        CHECK(target.parts.size() == 1);
        CHECK(target.parts[0].parameters.size() == 2);

        // The model edits the part as saved: without the blank row.
        CHECK(m.parameter_count() == 2);
        m.remove_parameter(0);
        CHECK(m.parameter_count() == 1);
        CHECK(m.parameter(0).name == "Resistance");
        m.remove_parameter(10); // nothing
        CHECK(m.parameter_count() == 1);

        // A known parameter comes with its unit, and only once.
        part_editor_model k;
        const std::size_t c = k.add_known_parameter("capacitance");
        CHECK(k.parameter(c).name == "Capacitance");
        CHECK(k.parameter(c).unit == "F");
        CHECK(k.add_known_parameter("Capacitance") == c);
        CHECK(k.parameter_count() == 1);
        CHECK(k.known_parameters().size() == library::schema::builtin().parameters().size());
    }

    void completion()
    {
        part_editor_model m;
        CHECK(m.complete_parameter("").empty());

        const auto res = m.complete_parameter("res");
        CHECK(!res.empty());
        CHECK(res.front().name == "Resistance");
        CHECK(res.front().unit == "\xCE\xA9");
        CHECK(std::ranges::any_of(res, [](const ui::parameter_completion &c) { return c.name == "Reverse voltage"; }) ==
              false); // "rev" is not "res"

        // By an alias, after the names.
        const auto ohm = m.complete_parameter("ohm");
        CHECK(!ohm.empty());
        CHECK(ohm.front().name == "Resistance");
        CHECK(ohm.front().alias == "Ohms");

        // Loosely: case, spaces and underscores do not count.
        const auto forward = m.complete_parameter("FORWARD_v");
        CHECK(!forward.empty() && forward.front().name == "Forward voltage");
        CHECK(m.complete_parameter("r", 2).size() == 2);

        m.add_parameter({"ohms", "10k", "", {}});
        m.add_parameter({"Resistence", "10k", "", {}});
        m.add_parameter({"Resistance", "10k", "\xCE\xA9", {}});
        m.add_parameter({"Capac", "", "", {}});
        m.add_parameter({"zzzzqqq", "1", "", {}});

        const auto alias = m.parameter_hints(0);
        CHECK(alias.size() == 1 && alias[0].name == "Resistance" && alias[0].alias == "Ohms");
        const auto misspelled = m.parameter_hints(1);
        CHECK(!misspelled.empty() && misspelled[0].name == "Resistance");
        CHECK(m.parameter_hints(2).empty());
        const auto typed = m.parameter_hints(3);
        CHECK(!typed.empty() && typed[0].name == "Capacitance" && typed[0].unit == "F");
        CHECK(m.parameter_hints(4).empty());
        CHECK(m.parameter_hints(99).empty());

        // The schema's issues follow the parameters by name.
        CHECK(has_issue(m.parameter_issues(1), library::issue_code::misspelled_parameter));
        CHECK(has_issue(m.parameter_issues(4), library::issue_code::unknown_parameter));

        // Taking a hint renames it and fills in an empty unit.
        m.apply_completion(1, misspelled[0]);
        CHECK(m.parameter(1).name == "Resistance");
        CHECK(m.parameter(1).unit == "\xCE\xA9");
        m.set_parameter_unit(3, "pF");
        m.apply_completion(3, typed[0]);
        CHECK(m.parameter(3).unit == "pF"); // its own unit kept
        CHECK(!has_issue(m.issues(), library::issue_code::misspelled_parameter));
        CHECK(has_issue(m.issues(), library::issue_code::duplicate_parameter));
    }

    void devices_and_checks()
    {
        part_editor_model m;
        const auto choices = m.device_choices();
        CHECK(choices.size() == library::schema::builtin().devices().size() + 1);
        CHECK(choices.front().id.empty() && choices.front().name == "None");
        CHECK(m.device_index() == 0);

        // The checks run as the part changes.
        m.set_field(part_field::name, "10k");
        CHECK(m.issues().empty());
        m.set_field(part_field::device, "resistor");
        CHECK(library::worst(m.issues()) == library::severity::error);
        CHECK(has_issue(m.issues(), library::issue_code::missing_parameter, "Resistance"));
        CHECK(choices[m.device_index()].id == "resistor");

        // Its required parameters, with their units.
        const auto missing = m.missing_parameters();
        CHECK(missing.size() == 1 && missing[0].name == "Resistance");
        CHECK(m.missing_parameters(false).size() == 3);
        CHECK(m.add_device_parameters() == 1);
        CHECK(m.parameter_count() == 1);
        CHECK(m.parameter(0).name == "Resistance");
        CHECK(m.parameter(0).unit == "\xCE\xA9");
        CHECK(m.missing_parameters().empty());
        CHECK(!has_issue(m.issues(), library::issue_code::missing_parameter));
        CHECK(has_issue(m.issues(), library::issue_code::missing_value, "Resistance"));
        CHECK(m.can_save()); // the schema's errors do not stop a save
        m.set_parameter_value(0, "10k");
        CHECK(library::worst(m.issues()) != library::severity::error);
        CHECK(m.add_device_parameters(false) == 2);
        CHECK(m.parameter_count() == 3);

        // A device found by an alias; one Volt does not know is offered too.
        m.set_field(part_field::device, "transistor");
        CHECK(m.device_choices()[m.device_index()].id == "bjt");
        m.set_field(part_field::device, "thyristor");
        CHECK(has_issue(m.issues(), library::issue_code::unknown_device));
        CHECK(m.device_choices().back().id == "thyristor");
        CHECK(m.device_index() == m.device_choices().size() - 1);
        CHECK(m.missing_parameters().empty());
    }

    void commit_new_and_edit()
    {
        library::library target = my_parts();

        part_editor_model m;
        m.set_field(part_field::name, "100nF 0402");
        m.set_field(part_field::category, " Passives / Capacitors/ ");
        m.set_field(part_field::mpn, "GRM155");
        auto added = m.commit(target);
        CHECK(added.has_value());
        const std::string id = added.value_or("");
        CHECK(target.parts.size() == 3);
        const library::part *p = target.find(id);
        CHECK(p != nullptr && p->category == "Passives/Capacitors");
        // From now on it edits that part.
        CHECK(m.mode() == editor_mode::edit);
        CHECK(m.original_id() == id);
        CHECK(!m.modified());

        // Edited in place, under its id.
        m.set_field(part_field::value, "100n");
        CHECK(m.modified());
        auto replaced = m.commit(target);
        CHECK(replaced && *replaced == id);
        CHECK(target.find(id) != nullptr && target.find(id)->value == "100n");
        CHECK(target.parts.size() == 3);
        CHECK(target.parts[2].id == id); // in its place

        // Editing a part that has gone: refused, and nothing changes.
        part_editor_model gone;
        gone.start_edit(make_part("nope", "Nope"));
        gone.set_field(part_field::value, "1");
        const library::library before = target;
        CHECK(!gone.commit(target).has_value());
        CHECK(target == before);

        // Everything the editor does not show is kept.
        library::part rich = make_part("rich", "Rich");
        rich.extra["future"] = json::value("kept");
        rich.origin = library::part_origin{"lib", "x", "1", "abc"};
        target.parts.push_back(rich);
        part_editor_model r;
        r.start_edit(rich);
        r.set_field(part_field::description, "now described");
        CHECK(r.commit(target).has_value());
        const library::part *kept = target.find("rich");
        CHECK(kept != nullptr && kept->extra == rich.extra && kept->origin == rich.origin);
        CHECK(kept != nullptr && kept->description == "now described");
    }

    void duplicate()
    {
        library::library source = library::new_library("volt-resistors", "Volt Resistors");
        source.version = "2026.11.0";
        library::part r = make_part("R_10k", "10k", "RC10K");
        r.model = "R_0603.step";
        source.parts.push_back(r);
        source.files.push_back({"R_0603.step", "ISO-10303-21;", {}});

        library::library target = library::new_library("my-parts", "My Parts");
        target.parts.push_back(make_part("R_10k", "Mine"));

        part_editor_model m;
        m.start_duplicate(r, source);
        CHECK(m.mode() == editor_mode::duplicate);
        CHECK(!m.modified());
        CHECK(m.part().id.empty()); // another part: an id of its own, when saved
        CHECK(m.can_save());
        CHECK(m.part().origin.has_value());

        auto saved = m.commit(target);
        CHECK(saved && *saved != "R_10k" && !saved->empty());
        const library::part *copy = saved ? target.find(*saved) : nullptr;
        CHECK(copy != nullptr);
        if (copy != nullptr)
        {
            CHECK(copy->origin.has_value());
            CHECK(copy->origin->library == "volt-resistors");
            CHECK(copy->origin->part == "R_10k");
            CHECK(copy->origin->version == "2026.11.0");
            CHECK(copy->origin->hash == library::part_hash(r));
            CHECK(copy->model == "R_0603.step");
        }
        CHECK(target.find_file("R_0603.step") != nullptr);

        // Duplicated again: another part again, the file carried once.
        part_editor_model again;
        again.start_duplicate(r, source);
        auto other = again.commit(target);
        CHECK(other && saved && *other != *saved);
        CHECK(target.parts.size() == 3);
        CHECK(target.files.size() == 1);
    }

    void model_files()
    {
        const std::filesystem::path dir = scratch("models");
        const std::filesystem::path step = dir / "C_0402.step";
        CHECK(json::write_text_file(step, "ISO-10303-21; model").has_value());

        part_editor_model m;
        m.set_field(part_field::name, "C");
        CHECK(m.attach_model(step).has_value());
        CHECK(m.field(part_field::model) == "C_0402.step");
        CHECK(m.attached_model().has_value());
        CHECK(m.modified());
        CHECK(!m.attach_model(dir / "missing.step").has_value());

        library::library target = library::new_library("mine");
        target.files.push_back({"C_0402.step", "something else", {}});
        auto saved = m.commit(target);
        CHECK(saved.has_value());
        // A different file of that name was there: renamed, and named so.
        CHECK(target.files.size() == 2);
        const library::part *c = saved ? target.find(*saved) : nullptr;
        CHECK(c != nullptr && c->model == "C_0402-2.step");
        CHECK(!m.attached_model().has_value());

        // Typing another name lets the file go.
        part_editor_model t;
        t.set_field(part_field::name, "T");
        CHECK(t.attach_model(step).has_value());
        t.set_field(part_field::model, "parts/other.step");
        CHECK(!t.attached_model().has_value());
        CHECK(t.can_save()); // a name under the models folder is not checked

        // An absolute path typed: read when saved, or a problem if absent.
        part_editor_model a;
        a.set_field(part_field::name, "A");
        a.set_field(part_field::model, library::path_to_utf8(dir / "nowhere.step"));
        CHECK(!a.can_save());
        a.set_field(part_field::model, library::path_to_utf8(step));
        CHECK(a.can_save());
        library::library other = library::new_library("other");
        auto typed = a.commit(other);
        CHECK(typed.has_value());
        const library::part *from_path = typed ? other.find(*typed) : nullptr;
        CHECK(from_path != nullptr && from_path->model == "C_0402.step");
        CHECK(other.find_file("C_0402.step") != nullptr);

        // A failed commit leaves no file behind.
        part_editor_model f;
        f.start_edit(make_part("gone", "Gone")); // not in `other`
        CHECK(f.attach_model(dir / "C_0402.step").has_value());
        library::library before = other;
        CHECK(!f.commit(other).has_value());
        CHECK(other == before);
    }

    void replaced_model_files()
    {
        const std::filesystem::path dir = scratch("replaced");
        CHECK(json::write_text_file(dir / "first.step", "ISO-10303-21; first").has_value());
        CHECK(json::write_text_file(dir / "second.step", "ISO-10303-21; second").has_value());

        // A file nothing named before: none of these edits' business.
        library::library target = library::new_library("mine");
        target.files.push_back({"stray.step", "stray", {}});

        part_editor_model a;
        a.set_field(part_field::name, "A");
        CHECK(a.attach_model(dir / "first.step").has_value());
        CHECK(a.commit(target).has_value());
        part_editor_model b;
        b.set_field(part_field::name, "B");
        b.set_field(part_field::model, "first.step"); // the file A brought
        CHECK(b.commit(target).has_value());
        CHECK(target.files.size() == 2);

        // A given another model: the old file stays while B names it.
        CHECK(a.attach_model(dir / "second.step").has_value());
        CHECK(a.commit(target).has_value());
        CHECK(target.find_file("first.step") != nullptr);
        CHECK(target.find_file("second.step") != nullptr);

        // B given none: nothing names the old file, which goes; the stray
        // one stays.
        b.set_field(part_field::model, "");
        CHECK(b.commit(target).has_value());
        CHECK(target.find_file("first.step") == nullptr);
        CHECK(target.find_file("stray.step") != nullptr);

        // A given a file under the models folder instead: its file goes.
        a.set_field(part_field::model, "parts/R_0603.step");
        CHECK(a.commit(target).has_value());
        CHECK(target.find_file("second.step") == nullptr);
        CHECK(target.files.size() == 1);

        // The helper by itself.
        CHECK(!ui::remove_file_if_unused(target, ""));
        CHECK(!ui::remove_file_if_unused(target, "not-there.step"));
        target.parts.push_back(make_part("named", "Named"));
        target.parts.back().model = "stray.step";
        CHECK(!ui::remove_file_if_unused(target, "stray.step"));
        target.parts.pop_back();
        CHECK(ui::remove_file_if_unused(target, "stray.step"));
        CHECK(target.files.empty());
    }

    void undo_fields()
    {
        part_editor_model m;
        CHECK(!m.can_undo() && !m.can_redo());
        CHECK(m.undo_label().empty() && m.redo_label().empty());
        CHECK(!m.undo() && !m.redo());

        // Typing into a field, a keystroke at a time: one step.
        m.set_field(part_field::name, "1");
        m.set_field(part_field::name, "10");
        m.set_field(part_field::name, "10k");
        CHECK(m.can_undo() && !m.can_redo());
        CHECK(m.undo_label() == "Edit name");
        CHECK(m.undo());
        CHECK(m.part().name.empty());
        CHECK(!m.can_undo() && m.can_redo());
        CHECK(m.redo_label() == "Edit name");
        CHECK(m.redo());
        CHECK(m.part().name == "10k");
        CHECK(!m.redo());

        // Another field is another step, and so is the first one again
        // after it.
        m.set_field(part_field::mpn, "R");
        m.set_field(part_field::mpn, "RC");
        CHECK(m.undo_label() == "Edit MPN");
        m.set_field(part_field::name, "10k 1%");
        CHECK(m.undo());
        CHECK(m.part().name == "10k" && m.part().mpn == "RC");
        CHECK(m.undo());
        CHECK(m.part().mpn.empty() && m.part().name == "10k");
        CHECK(m.undo());
        CHECK(m.part().name.empty());
        CHECK(!m.can_undo());

        // An edit after an undo: the step undone cannot be redone, and
        // typing on is a step of its own, not the one undone.
        CHECK(m.redo());
        CHECK(m.part().name == "10k");
        m.set_field(part_field::category, "Passives");
        CHECK(m.undo());
        m.set_field(part_field::category, "Resistors");
        CHECK(!m.can_redo());
        CHECK(m.part().category == "Resistors");
        CHECK(m.undo());
        CHECK(m.part().category.empty() && m.part().name == "10k");

        // seal() ends the typing (the field lost the focus).
        part_editor_model s;
        s.set_field(part_field::value, "10");
        s.seal();
        s.set_field(part_field::value, "10k");
        CHECK(s.undo());
        CHECK(s.part().value == "10");
        CHECK(s.undo());
        CHECK(s.part().value.empty());

        // Labels by field.
        part_editor_model l;
        l.set_field(part_field::model, "parts/R.step");
        CHECK(l.undo_label() == "Edit 3D model");
        l.set_field(part_field::keywords, "a, b");
        CHECK(l.undo_label() == "Edit keywords");
        l.set_field(part_field::device, "resistor");
        CHECK(l.undo_label() == "Choose device");
    }

    void undo_no_ops()
    {
        part_editor_model m;
        m.set_field(part_field::name, "R1");
        const std::uint64_t revision = m.revision();
        m.seal();

        // The text it has, the id (not typed), keywords that read the same:
        // no step, and nothing to tell a window.
        m.set_field(part_field::name, "R1");
        m.set_field(part_field::id, "typed");
        m.set_field(part_field::keywords, "");
        m.set_field(part_field::keywords, " , ");
        CHECK(m.revision() == revision);
        CHECK(m.undo());
        CHECK(!m.can_undo());

        // Parameters: a value it has, a row out of range, a known parameter
        // the part has already, a move to where it is.
        m.set_field(part_field::name, "R1");
        const std::size_t r = m.add_known_parameter("resistance");
        CHECK(m.undo_label() == "Add parameter");
        m.set_parameter_value(r, "10k");
        const std::uint64_t before = m.revision();
        m.set_parameter_value(r, "10k");
        m.set_parameter_name(9, "x");
        m.remove_parameter(9);
        CHECK(m.add_known_parameter("Resistance") == r);
        CHECK(!m.move_parameter(0, 0));
        CHECK(m.add_device_parameters() == 0); // no device
        CHECK(m.revision() == before);
        CHECK(m.undo_label() == "Edit parameter value");

        // Typed and typed back: no step left, nothing for Undo to do.
        part_editor_model t;
        t.set_field(part_field::name, "x");
        t.set_field(part_field::name, "");
        CHECK(!t.modified());
        CHECK(!t.can_undo() && t.undo_label().empty());

        // Across fields: the step typed back goes, the one before it stays,
        // finished: typing into its field again is a step of its own.
        t.set_field(part_field::name, "a");
        t.set_field(part_field::mpn, "b");
        t.set_field(part_field::mpn, "");
        CHECK(t.undo_label() == "Edit name");
        CHECK(!t.can_redo());
        t.set_field(part_field::name, "ab");
        CHECK(t.undo());
        CHECK(t.part().name == "a");
        CHECK(t.undo());
        CHECK(t.part().name.empty() && !t.can_undo());

        // A parameter's value typed back: the row's step stays.
        part_editor_model v;
        v.set_field(part_field::mpn, "RC0603");
        v.add_parameter({"Resistance", "10k", "\xCE\xA9", {}});
        v.set_parameter_value(0, "10k5");
        v.set_parameter_value(0, "10k");
        CHECK(v.undo_label() == "Add parameter");

        // After a save: typed and typed back is unmodified, with the steps
        // before the save still there to undo.
        library::library target = library::new_library("mine");
        CHECK(v.commit(target).has_value());
        v.set_field(part_field::description, "R");
        CHECK(v.modified());
        v.set_field(part_field::description, "");
        CHECK(!v.modified());
        CHECK(v.undo_label() == "Add parameter" && !v.can_redo());

        // Equal rows swapped: the move was made, but it is no step.
        part_editor_model e;
        e.add_parameter();
        e.add_parameter();
        e.seal();
        const std::uint64_t rows = e.revision();
        CHECK(e.move_parameter(0, 1));
        CHECK(e.revision() == rows);
    }

    void undo_parameters()
    {
        part_editor_model m;
        m.set_field(part_field::name, "R");

        // Added: a step; its fields typed, each its own step.
        CHECK(m.add_parameter() == 0);
        CHECK(m.undo_label() == "Add parameter");
        m.set_parameter_name(0, "R");
        m.set_parameter_name(0, "Res");
        CHECK(m.undo_label() == "Edit parameter name");
        m.set_parameter_value(0, "1");
        m.set_parameter_value(0, "10k");
        CHECK(m.undo_label() == "Edit parameter value");
        m.set_parameter_unit(0, "\xCE\xA9");
        CHECK(m.undo_label() == "Edit parameter unit");
        m.set_parameter_name(0, "Resist");
        CHECK(m.undo());
        CHECK(m.parameter(0).name == "Res");
        CHECK(m.undo());
        CHECK(m.parameter(0).unit.empty() && m.parameter(0).value == "10k");
        CHECK(m.undo());
        CHECK(m.parameter(0).value.empty());
        CHECK(m.undo());
        CHECK(m.parameter(0).name.empty());
        CHECK(m.undo());
        CHECK(m.parameter_count() == 0);
        while (m.redo())
        {
        }
        CHECK(m.parameter_count() == 1 && m.parameter(0).name == "Resist" && m.parameter(0).unit == "\xCE\xA9");

        // The same field of another row is another step.
        m.add_parameter({"Tolerance", "", "%", {}});
        m.set_parameter_value(1, "1");
        m.seal();
        m.set_parameter_value(0, "4k7");
        CHECK(m.undo());
        CHECK(m.parameter(0).value == "10k" && m.parameter(1).value == "1");

        // Moved, removed: undone, the rows are as they were, in order.
        CHECK(m.move_parameter(1, 0));
        CHECK(m.undo_label() == "Move parameter");
        CHECK(m.parameter(0).name == "Tolerance");
        CHECK(m.undo());
        CHECK(m.parameter(0).name == "Resist" && m.parameter(1).name == "Tolerance");
        m.remove_parameter(0);
        CHECK(m.undo_label() == "Remove parameter");
        CHECK(m.parameter_count() == 1);
        CHECK(m.undo());
        CHECK(m.parameter_count() == 2 && m.parameter(0).name == "Resist" && m.parameter(0).value == "10k");

        // A completion taken: name and unit, one step.
        CHECK(has_issue(m.issues(), library::issue_code::misspelled_parameter, "Resist") ||
              has_issue(m.issues(), library::issue_code::unknown_parameter, "Resist"));
        m.set_parameter_unit(0, "");
        m.apply_completion(0, {"Resistance", "\xCE\xA9", {}});
        CHECK(m.undo_label() == "Rename parameter");
        CHECK(m.parameter(0).name == "Resistance" && m.parameter(0).unit == "\xCE\xA9");
        CHECK(m.undo());
        CHECK(m.parameter(0).name == "Resist" && m.parameter(0).unit.empty());
        // The checks are those of the part as it is again.
        CHECK(has_issue(m.issues(), library::issue_code::misspelled_parameter, "Resist") ||
              has_issue(m.issues(), library::issue_code::unknown_parameter, "Resist"));
        CHECK(m.redo());
        CHECK(!has_issue(m.issues(), library::issue_code::misspelled_parameter));

        // A known parameter: one step, with its unit.
        const std::size_t c = m.add_known_parameter("capacitance");
        CHECK(m.parameter(c).unit == "F");
        CHECK(m.undo_label() == "Add parameter");
        CHECK(m.undo());
        CHECK(m.parameter_count() == 2);
    }

    void undo_device_and_checks()
    {
        part_editor_model m;
        m.set_field(part_field::name, "10k");
        CHECK(m.issues().empty());

        // Each device chosen is a step: chosen, not typed.
        m.set_field(part_field::device, "resistor");
        m.set_field(part_field::device, "capacitor");
        CHECK(m.undo_label() == "Choose device");
        CHECK(m.undo());
        CHECK(m.part().device == "resistor");
        CHECK(has_issue(m.issues(), library::issue_code::missing_parameter, "Resistance"));

        // Its parameters, added by one click: one step, undone together,
        // and the checks with them.
        CHECK(m.add_device_parameters(false) == 3);
        CHECK(m.undo_label() == "Add device parameters");
        CHECK(!has_issue(m.issues(), library::issue_code::missing_parameter));
        CHECK(m.undo());
        CHECK(m.parameter_count() == 0);
        CHECK(has_issue(m.issues(), library::issue_code::missing_parameter, "Resistance"));
        CHECK(m.redo());
        CHECK(m.parameter_count() == 3);
        CHECK(!has_issue(m.issues(), library::issue_code::missing_parameter));

        // The device undone: no device, no issues, the choice None.
        CHECK(m.undo());
        CHECK(m.undo());
        CHECK(m.part().device.empty());
        CHECK(m.device_index() == 0);
        CHECK(m.issues().empty());
    }

    void undo_model_file()
    {
        const std::filesystem::path dir = scratch("undo");
        const std::filesystem::path step = dir / "C_0402.step";
        CHECK(json::write_text_file(step, "ISO-10303-21; model").has_value());

        part_editor_model m;
        m.set_field(part_field::name, "C");
        m.set_field(part_field::model, "parts/C.step");
        CHECK(m.attach_model(step).has_value());
        CHECK(m.undo_label() == "Attach model");
        CHECK(m.attached_model().has_value());

        // Undone: the file is let go, and the name typed before is back.
        CHECK(m.undo());
        CHECK(!m.attached_model().has_value());
        CHECK(m.field(part_field::model) == "parts/C.step");
        CHECK(m.redo());
        CHECK(m.attached_model().has_value() && m.attached_model()->name == "C_0402.step");
        CHECK(m.field(part_field::model) == "C_0402.step");

        // The same file again is nothing new.
        const std::uint64_t revision = m.revision();
        CHECK(m.attach_model(step).has_value());
        CHECK(m.revision() == revision);

        // Typing another name lets the file go; undone, it is back.
        m.set_field(part_field::model, "parts/other.step");
        CHECK(!m.attached_model().has_value());
        CHECK(m.undo());
        CHECK(m.attached_model().has_value());
        CHECK(m.field(part_field::model) == "C_0402.step");
        CHECK(m.can_save());

        // A failed attach is no step.
        CHECK(!m.attach_model(dir / "missing.step").has_value());
        CHECK(m.undo_label() == "Attach model");

        // Saved, the file is the library's; undone past the save, it is
        // attached again, to go in with the next save.
        m.set_field(part_field::description, "Chip capacitor");
        library::library target = library::new_library("mine");
        auto saved = m.commit(target);
        CHECK(saved.has_value());
        CHECK(target.find_file("C_0402.step") != nullptr);
        CHECK(!m.attached_model().has_value() && !m.modified());
        CHECK(m.undo_label() == "Edit description");
        CHECK(m.undo());
        CHECK(m.attached_model().has_value() && m.field(part_field::model) == "C_0402.step");
        CHECK(m.modified());
        CHECK(m.redo());
        CHECK(!m.attached_model().has_value());
        CHECK(!m.modified());

        // And before it was attached: the name typed then.
        CHECK(m.undo());
        CHECK(m.undo());
        CHECK(!m.attached_model().has_value() && m.field(part_field::model) == "parts/C.step");
        CHECK(m.modified());
    }

    void undo_across_commit()
    {
        library::library target = my_parts();

        part_editor_model m;
        m.set_field(part_field::name, "100nF");
        m.set_field(part_field::category, "Capacitors");
        m.add_parameter(); // a blank row: the save leaves it out
        auto added = m.commit(target);
        CHECK(added.has_value());
        const std::string id = added.value_or("");
        CHECK(!m.modified());
        CHECK(m.part() == *target.find(id));
        CHECK(m.parameter_count() == 0);

        // The history is kept; the row the save left out is no step.
        CHECK(m.can_undo() && !m.can_redo());
        CHECK(m.undo_label() == "Edit category");

        // Typing after the save is a step of its own: undone, the part is
        // as saved, and not modified.
        m.set_field(part_field::name, "100nF 0402");
        CHECK(m.modified());
        CHECK(m.undo_label() == "Edit name");
        CHECK(m.undo());
        CHECK(m.part().name == "100nF");
        CHECK(m.part() == *target.find(id));
        CHECK(!m.modified());

        // Past the save: modified, under the id the part was given.
        CHECK(m.undo());
        CHECK(m.part().category.empty());
        CHECK(m.modified());
        CHECK(m.part().id == id && m.field(part_field::id) == id);
        CHECK(m.mode() == editor_mode::edit && m.original_id() == id);
        CHECK(m.undo());
        CHECK(m.part().name.empty());
        CHECK(!m.can_undo());

        // Back to the save: the part as the library has it, unmodified.
        CHECK(m.redo());
        CHECK(m.redo());
        CHECK(m.part() == *target.find(id));
        CHECK(!m.modified());
        CHECK(m.redo());
        CHECK(m.part().name == "100nF 0402" && m.modified());

        // Saved again from a state before the first save: replaces the part.
        CHECK(m.undo());
        CHECK(m.undo());
        CHECK(m.modified());
        auto again = m.commit(target);
        CHECK(again && *again == id);
        CHECK(target.parts.size() == 3);
        CHECK(target.find(id)->category.empty());
        CHECK(!m.modified());
        // The step after it was undone, and can still be redone.
        CHECK(m.can_redo() && m.redo_label() == "Edit category");
        CHECK(m.redo());
        CHECK(m.modified());

        // What the library made of the part (a category normalized) is the
        // saved state: redone back to it, the part is as the library has
        // it, not as it was typed.
        part_editor_model n;
        n.set_field(part_field::name, "1uF");
        n.set_field(part_field::category, " Passives / Capacitors/ ");
        auto normalized = n.commit(target);
        CHECK(normalized.has_value());
        CHECK(n.part().category == "Passives/Capacitors");
        CHECK(n.undo_label() == "Edit category");
        CHECK(n.undo());
        CHECK(n.part().category.empty() && n.modified());
        CHECK(n.redo());
        CHECK(n.part().category == "Passives/Capacitors");
        CHECK(!n.modified());

        // A failed commit leaves the history as it was.
        part_editor_model gone;
        gone.start_edit(make_part("nope", "Nope"));
        gone.set_field(part_field::value, "1");
        CHECK(!gone.commit(target).has_value());
        CHECK(gone.undo_label() == "Edit value");
        CHECK(gone.modified());
        CHECK(gone.undo());
        CHECK(!gone.modified());
    }

    void undo_history_reset()
    {
        library::library source = my_parts();
        part_editor_model m;
        m.set_field(part_field::name, "x");
        CHECK(m.undo());
        CHECK(m.can_redo());

        m.start_edit(source.parts[0]);
        CHECK(!m.can_undo() && !m.can_redo() && !m.modified());
        m.set_field(part_field::value, "10k");
        m.start_duplicate(source.parts[1], source);
        CHECK(!m.can_undo() && !m.can_redo() && !m.modified());
        m.set_field(part_field::value, "dual");
        m.start_new();
        CHECK(!m.can_undo() && !m.can_redo() && !m.modified());
        // Begun again: typing into the field of the last part is a new step.
        m.set_field(part_field::value, "x");
        CHECK(m.undo());
        CHECK(!m.can_undo());
    }

    void undo_copies()
    {
        part_editor_model m;
        m.set_field(part_field::name, "a");
        m.set_field(part_field::mpn, "b");
        m.set_field(part_field::mpn, "bc");

        // A copy has the same steps, its own from then on.
        part_editor_model c = m;
        CHECK(c.undo_label() == "Edit MPN");
        CHECK(c.undo());
        CHECK(c.part().mpn.empty());
        CHECK(m.part().mpn == "bc" && m.can_undo() && !m.can_redo());
        CHECK(c.redo());
        CHECK(c.part() == m.part());

        // Typing goes on into the step being typed in the copy too.
        part_editor_model d = m;
        d.set_field(part_field::mpn, "bcd");
        CHECK(d.undo());
        CHECK(d.part().mpn.empty());
        CHECK(d.undo_label() == "Edit name");

        // Assigned, likewise; the saved state comes along.
        library::library target = library::new_library("mine");
        CHECK(m.commit(target).has_value());
        part_editor_model e;
        e = m;
        CHECK(!e.modified() && e.can_undo());
        e.set_field(part_field::mpn, "bcde");
        CHECK(e.undo());
        CHECK(!e.modified());
    }

    void shipped_libraries()
    {
        const std::filesystem::path root = scratch("shipped");
        const std::filesystem::path assets = root / "assets";
        std::filesystem::create_directories(assets / "libraries");
        std::filesystem::create_directories(root / "user");

        CHECK(ui::is_shipped_library(assets / "libraries" / "volt-resistors.voltlib", assets));
        CHECK(ui::is_shipped_library(assets / "libraries" / "sub" / "x.voltlib", assets / ""));
        CHECK(ui::is_shipped_library(assets / "libraries" / ".." / "libraries" / "x.voltlib", assets));
        CHECK(!ui::is_shipped_library(root / "user" / "my-parts.voltlib", assets));
        CHECK(!ui::is_shipped_library(root / "assets-other" / "x.voltlib", assets));
        CHECK(!ui::is_shipped_library({}, assets));
        CHECK(!ui::is_shipped_library(assets / "libraries" / "x.voltlib", {}));

        CHECK(ui::is_writable_library(root / "user" / "my-parts.voltlib", assets));
        CHECK(!ui::is_writable_library(assets / "libraries" / "x.voltlib", assets));
        CHECK(!ui::is_writable_library({}, assets));

        // Relative paths, against the same working directory.
        const std::filesystem::path previous = std::filesystem::current_path();
        std::filesystem::current_path(root);
        CHECK(ui::is_shipped_library(std::filesystem::path("assets") / "libraries" / "a.voltlib", "assets"));
        CHECK(ui::is_shipped_library(assets / "libraries" / "a.voltlib", "assets"));
        CHECK(!ui::is_shipped_library(std::filesystem::path("user") / "a.voltlib", "assets"));
        std::filesystem::current_path(previous);
    }
} // namespace

int main()
{
    keywords_and_titles();
    ids();
    parameters();
    completion();
    devices_and_checks();
    commit_new_and_edit();
    duplicate();
    model_files();
    replaced_model_files();
    undo_fields();
    undo_no_ops();
    undo_parameters();
    undo_device_and_checks();
    undo_model_file();
    undo_across_commit();
    undo_history_reset();
    undo_copies();
    shipped_libraries();
    return volt::tests::result("part editor model");
}
