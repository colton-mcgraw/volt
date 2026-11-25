#include <app/ui/windows/part_editor_model.hpp>

#include <library/edit.hpp>
#include <library/project.hpp>

#include <algorithm>
#include <format>
#include <system_error>
#include <utility>

namespace volt::ui
{
    namespace
    {
        bool is_space(char c) noexcept
        {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r';
        }

        std::string_view trim(std::string_view text) noexcept
        {
            while (!text.empty() && is_space(text.front()))
                text.remove_prefix(1);
            while (!text.empty() && is_space(text.back()))
                text.remove_suffix(1);
            return text;
        }

        // A name as the schema compares names: ASCII case folded, and
        // spaces, '_', '-' and '.' left out. Only for completing what is
        // typed; the schema matches names itself.
        std::string fold(std::string_view text)
        {
            std::string folded;
            folded.reserve(text.size());
            for (const char c : text)
            {
                if (c == ' ' || c == '_' || c == '-' || c == '.')
                    continue;
                folded += c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
            }
            return folded;
        }

        bool starts_with(std::string_view text, std::string_view prefix) noexcept
        {
            return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
        }

        bool blank(const library::parameter &p) noexcept
        {
            return trim(p.name).empty() && trim(p.value).empty() && trim(p.unit).empty();
        }

        // An absolute path to a file the user typed into the model field:
        // read and carried into the library, rather than named as a path
        // under the models directory.
        std::optional<std::filesystem::path> typed_model_file(std::string_view model)
        {
            const std::string_view text = trim(model);
            if (text.empty())
                return std::nullopt;
            std::filesystem::path path = library::path_from_utf8(text);
            if (!path.is_absolute())
                return std::nullopt;
            return path;
        }

        std::filesystem::path normal_path(const std::filesystem::path &path)
        {
            std::error_code error;
            std::filesystem::path absolute = std::filesystem::weakly_canonical(path, error);
            if (error)
            {
                absolute = std::filesystem::absolute(path, error);
                if (error)
                    absolute = path;
            }
            absolute = absolute.lexically_normal();
            // "assets/" and "assets" alike: a trailing separator is an empty
            // last element, which no file below it has.
            if (!absolute.has_filename() && absolute.has_parent_path() && absolute != absolute.root_path())
                absolute = absolute.parent_path();
            return absolute;
        }

        // "Edit name", "Edit MPN", "Edit 3D model": the step a field's edit
        // is, for the Undo menu. A title's first letter goes to lower case
        // unless the title is an abbreviation.
        std::string field_step(part_field f)
        {
            if (f == part_field::device)
                return "Choose device"; // chosen from a list, not typed
            std::string title(field_title(f));
            if (title.size() > 1 && title[0] >= 'A' && title[0] <= 'Z' && !(title[1] >= 'A' && title[1] <= 'Z'))
                title[0] = static_cast<char>(title[0] - 'A' + 'a');
            return "Edit " + title;
        }

        // What makes typing into one field one step: the same key for each
        // keystroke, another for another field (stack::commit's merge key).
        std::string typing_key(std::string_view what, std::size_t index)
        {
            return std::format("{}:{}", what, index);
        }
    } // namespace

    // ---- fields ---------------------------------------------------------------------------------

    std::string_view field_title(part_field field) noexcept
    {
        switch (field)
        {
        case part_field::name:
            return "Name";
        case part_field::id:
            return "ID";
        case part_field::category:
            return "Category";
        case part_field::description:
            return "Description";
        case part_field::manufacturer:
            return "Manufacturer";
        case part_field::mpn:
            return "MPN";
        case part_field::value:
            return "Value";
        case part_field::package:
            return "Package";
        case part_field::prefix:
            return "Prefix";
        case part_field::device:
            return "Device";
        case part_field::symbol:
            return "Symbol";
        case part_field::footprint:
            return "Footprint";
        case part_field::model:
            return "3D model";
        case part_field::datasheet:
            return "Datasheet";
        case part_field::keywords:
            return "Keywords";
        }
        return "";
    }

    std::string_view field_placeholder(part_field field) noexcept
    {
        switch (field)
        {
        case part_field::name:
            return "10k\xE2\x84\xA6 1% 0603";
        case part_field::id:
            return "Given when the part is saved";
        case part_field::category:
            return "Passives/Resistors/Chip";
        case part_field::description:
            return "Thick film chip resistor";
        case part_field::manufacturer:
            return "Yageo";
        case part_field::mpn:
            return "RC0603FR-0710KL";
        case part_field::value:
            return "10k";
        case part_field::package:
            return "0603";
        case part_field::prefix:
            return "R";
        case part_field::device:
            return "";
        case part_field::symbol:
            return "R";
        case part_field::footprint:
            return "Resistor_SMD:R_0603_1608Metric";
        case part_field::model:
            return "A file in the models folder, or drop one on the window";
        case part_field::datasheet:
            return "https://";
        case part_field::keywords:
            return "resistor, chip, smd";
        }
        return "";
    }

    std::vector<std::string> parse_keywords(std::string_view text)
    {
        std::vector<std::string> keywords;
        while (true)
        {
            const std::size_t comma = text.find(',');
            const std::string_view keyword = trim(text.substr(0, comma));
            if (!keyword.empty())
                keywords.emplace_back(keyword);
            if (comma == std::string_view::npos)
                break;
            text.remove_prefix(comma + 1);
        }
        return keywords;
    }

    std::string join_keywords(std::span<const std::string> keywords)
    {
        std::string text;
        for (const std::string &keyword : keywords)
        {
            if (!text.empty())
                text += ", ";
            text += keyword;
        }
        return text;
    }

    bool is_shipped_library(const std::filesystem::path &file, const std::filesystem::path &assets)
    {
        if (file.empty() || assets.empty())
            return false;
        const std::filesystem::path relative = normal_path(file).lexically_relative(normal_path(assets));
        if (relative.empty())
            return false; // another drive, or nothing in common
        const std::filesystem::path first = *relative.begin();
        return first != ".." && first != ".";
    }

    bool is_writable_library(const std::filesystem::path &file, const std::filesystem::path &assets)
    {
        return !file.empty() && !is_shipped_library(file, assets);
    }

    bool remove_file_if_unused(library::library &lib, std::string_view name)
    {
        if (name.empty() || lib.find_file(name) == nullptr)
            return false;
        if (std::ranges::any_of(lib.parts, [&](const library::part &p) { return p.model == name; }))
            return false;
        std::erase_if(lib.files, [&](const library::embedded_file &f) { return f.name == name; });
        return true;
    }

    // ---- the model ------------------------------------------------------------------------------

    part_editor_model::part_editor_model(const library::schema &schema) : m_schema(&schema)
    {
        start_new();
    }

    void part_editor_model::start(editor_mode mode, library::part part)
    {
        m_mode = mode;
        m_doc = {std::move(part), nullptr};
        // Only a part being edited has an id: the others are given theirs
        // by the library they go into.
        if (mode != editor_mode::edit)
            m_doc.part.id.clear();
        m_original_id = m_doc.part.id;
        m_carried.clear();
        m_baseline = m_doc;
        // Another part, or the same afresh: nothing before this is undone.
        m_history.clear();
        m_history.mark_saved();
        changed();
    }

    void part_editor_model::start_new()
    {
        start(editor_mode::create, {});
    }

    void part_editor_model::start_edit(const library::part &part)
    {
        start(editor_mode::edit, part);
    }

    void part_editor_model::start_duplicate(const library::part &part, const library::library &source)
    {
        // Linked to where it came from, as a project's copies are, so that
        // it can be compared with its source later; another part, so not
        // under its id.
        library::part copy = part;
        copy.origin = library::part_origin{source.name, part.id, source.version, library::part_hash(part)};
        start(editor_mode::duplicate, std::move(copy));
        if (const library::embedded_file *file = source.find_file(part.model); file != nullptr)
            m_carried.push_back(*file);
    }

    bool part_editor_model::modified() const
    {
        // Compared rather than taken from the history's saved marker, which
        // knows positions, not parts: a name changed, left, and changed back
        // later is two steps away from the save and still the saved part;
        // so is one reached again after an undo past the save and a new
        // edit, which leave the marker nowhere. A part is small to compare.
        // The baseline has no attached file, so one attached is always a
        // change, as it must be: it is not in the library yet.
        return m_doc != m_baseline;
    }

    // ---- undo -----------------------------------------------------------------------------------

    bool part_editor_model::undo()
    {
        if (!m_history.undo(m_doc))
            return false;
        changed();
        return true;
    }

    bool part_editor_model::redo()
    {
        if (!m_history.redo(m_doc))
            return false;
        changed();
        return true;
    }

    void part_editor_model::edit(std::string label, document next, std::string merge_key)
    {
        auto step = history::make_value_change(m_doc, std::move(next));
        if (!step)
            return; // nothing changed: nothing to undo
        m_history.perform(m_doc, std::move(label), std::move(*step), std::move(merge_key));
        changed();
    }

    library::part part_editor_model::cleaned() const
    {
        library::part p = m_doc.part;
        std::erase_if(p.parameters, blank);
        for (library::parameter &q : p.parameters)
        {
            q.name = std::string(trim(q.name));
            q.value = std::string(trim(q.value));
            q.unit = std::string(trim(q.unit));
        }
        return p;
    }

    void part_editor_model::changed()
    {
        // A row being filled in has no name yet; what is wrong with that
        // is a save problem, not something for the schema to puzzle over.
        library::part checked = cleaned();
        std::erase_if(checked.parameters, [](const library::parameter &q) { return q.name.empty(); });
        m_issues = m_schema->check(checked);
    }

    // ---- fields ---------------------------------------------------------------------------------

    std::string part_editor_model::field(part_field f) const
    {
        const library::part &p = m_doc.part;
        switch (f)
        {
        case part_field::name:
            return p.name;
        case part_field::id:
            return p.id;
        case part_field::category:
            return p.category;
        case part_field::description:
            return p.description;
        case part_field::manufacturer:
            return p.manufacturer;
        case part_field::mpn:
            return p.mpn;
        case part_field::value:
            return p.value;
        case part_field::package:
            return p.package;
        case part_field::prefix:
            return p.prefix;
        case part_field::device:
            return p.device;
        case part_field::symbol:
            return p.symbol;
        case part_field::footprint:
            return p.footprint;
        case part_field::model:
            return p.model;
        case part_field::datasheet:
            return p.datasheet;
        case part_field::keywords:
            return join_keywords(p.keywords);
        }
        return {};
    }

    void part_editor_model::set_field(part_field f, std::string_view text)
    {
        document next = m_doc;
        library::part &p = next.part;
        std::string value(text);
        switch (f)
        {
        case part_field::name:
            p.name = std::move(value);
            break;
        case part_field::id:
            return; // the part's identity: not typed
        case part_field::category:
            p.category = std::move(value);
            break;
        case part_field::description:
            p.description = std::move(value);
            break;
        case part_field::manufacturer:
            p.manufacturer = std::move(value);
            break;
        case part_field::mpn:
            p.mpn = std::move(value);
            break;
        case part_field::value:
            p.value = std::move(value);
            break;
        case part_field::package:
            p.package = std::move(value);
            break;
        case part_field::prefix:
            p.prefix = std::move(value);
            break;
        case part_field::device:
            p.device = std::move(value);
            break;
        case part_field::symbol:
            p.symbol = std::move(value);
            break;
        case part_field::footprint:
            p.footprint = std::move(value);
            break;
        case part_field::model:
            p.model = std::move(value);
            // The file given goes with the name it was given under; typing
            // another name lets it go.
            if (next.model_file && (*next.model_file)->name != p.model)
                next.model_file.reset();
            break;
        case part_field::datasheet:
            p.datasheet = std::move(value);
            break;
        case part_field::keywords:
            p.keywords = parse_keywords(value);
            break;
        }
        // Typed, keystroke by keystroke: one step while it is the same
        // field. The device is chosen, each choice a step.
        edit(field_step(f), std::move(next),
             f == part_field::device ? std::string() : typing_key("field", static_cast<std::size_t>(f)));
    }

    // ---- parameters -----------------------------------------------------------------------------

    std::size_t part_editor_model::add_parameter(library::parameter p)
    {
        document next = m_doc;
        next.part.parameters.push_back(std::move(p));
        edit("Add parameter", std::move(next));
        return m_doc.part.parameters.size() - 1;
    }

    std::size_t part_editor_model::add_known(library::part &p, std::string_view name) const
    {
        const library::parameter_definition *definition = m_schema->find(name);
        if (definition == nullptr)
        {
            p.parameters.push_back({std::string(trim(name)), {}, {}, {}});
            return p.parameters.size() - 1;
        }
        if (const library::parameter *there = m_schema->find_parameter(p, definition->name); there != nullptr)
            return static_cast<std::size_t>(there - p.parameters.data());
        p.parameters.push_back({definition->name, {}, definition->unit, {}});
        return p.parameters.size() - 1;
    }

    std::size_t part_editor_model::add_known_parameter(std::string_view name)
    {
        document next = m_doc;
        const std::size_t index = add_known(next.part, name);
        edit("Add parameter", std::move(next));
        return index;
    }

    void part_editor_model::remove_parameter(std::size_t index)
    {
        if (index >= m_doc.part.parameters.size())
            return;
        document next = m_doc;
        next.part.parameters.erase(next.part.parameters.begin() + static_cast<std::ptrdiff_t>(index));
        edit("Remove parameter", std::move(next));
    }

    bool part_editor_model::move_parameter(std::size_t from, std::size_t to)
    {
        const std::size_t count = m_doc.part.parameters.size();
        if (from >= count || to >= count || from == to)
            return false;
        document next = m_doc;
        std::vector<library::parameter> &list = next.part.parameters;
        library::parameter moving = std::move(list[from]);
        list.erase(list.begin() + static_cast<std::ptrdiff_t>(from));
        list.insert(list.begin() + static_cast<std::ptrdiff_t>(to), std::move(moving));
        // Two equal rows swapped change nothing, and are no step; the move
        // was still made.
        edit("Move parameter", std::move(next));
        return true;
    }

    // Each of a parameter's three fields is typed into as a field of the
    // part is: one step while it is the same field of the same row. Rows
    // are keyed by position, which is safe: adding, removing or moving a
    // row is a step of its own in between, which ends the typing.

    void part_editor_model::set_parameter_name(std::size_t index, std::string_view name)
    {
        if (index >= m_doc.part.parameters.size())
            return;
        document next = m_doc;
        next.part.parameters[index].name = std::string(name);
        edit("Edit parameter name", std::move(next), typing_key("parameter name", index));
    }

    void part_editor_model::set_parameter_value(std::size_t index, std::string_view value)
    {
        if (index >= m_doc.part.parameters.size())
            return;
        document next = m_doc;
        next.part.parameters[index].value = std::string(value);
        edit("Edit parameter value", std::move(next), typing_key("parameter value", index));
    }

    void part_editor_model::set_parameter_unit(std::size_t index, std::string_view unit)
    {
        if (index >= m_doc.part.parameters.size())
            return;
        document next = m_doc;
        next.part.parameters[index].unit = std::string(unit);
        edit("Edit parameter unit", std::move(next), typing_key("parameter unit", index));
    }

    std::vector<parameter_completion> part_editor_model::complete_parameter(std::string_view typed,
                                                                            std::size_t limit) const
    {
        const std::string wanted = fold(trim(typed));
        if (wanted.empty() || limit == 0)
            return {};

        std::vector<parameter_completion> by_name;
        std::vector<parameter_completion> by_alias;
        for (const library::parameter_definition &d : m_schema->parameters())
        {
            if (starts_with(fold(d.name), wanted))
            {
                by_name.push_back({d.name, d.unit, {}});
                continue;
            }
            for (const std::string &alias : d.aliases)
            {
                if (starts_with(fold(alias), wanted))
                {
                    by_alias.push_back({d.name, d.unit, alias});
                    break;
                }
            }
        }
        by_name.insert(by_name.end(), by_alias.begin(), by_alias.end());
        if (by_name.size() > limit)
            by_name.resize(limit);
        return by_name;
    }

    std::vector<parameter_completion> part_editor_model::known_parameters() const
    {
        std::vector<parameter_completion> known;
        for (const library::parameter_definition &d : m_schema->parameters())
            known.push_back({d.name, d.unit, {}});
        return known;
    }

    std::vector<parameter_completion> part_editor_model::parameter_hints(std::size_t index, std::size_t limit) const
    {
        if (index >= m_doc.part.parameters.size() || limit == 0)
            return {};
        const std::string_view name = trim(m_doc.part.parameters[index].name);
        if (name.empty())
            return {};

        // Known: nothing to say if written as the schema writes it,
        // otherwise its proper name.
        if (const library::parameter_definition *d = m_schema->find(name); d != nullptr)
        {
            if (d->name == name)
                return {};
            std::string alias;
            for (const std::string &a : d->aliases)
                if (fold(a) == fold(name))
                    alias = a;
            return {{d->name, d->unit, std::move(alias)}};
        }

        // Being typed: the names it starts.
        if (std::vector<parameter_completion> started = complete_parameter(name, limit); !started.empty())
            return started;

        // Misspelled: the names it is close to.
        std::vector<parameter_completion> close;
        for (const std::string &suggested : m_schema->suggest(name, limit))
        {
            const library::parameter_definition *d = m_schema->find(suggested);
            close.push_back({suggested, d != nullptr ? d->unit : std::string(), {}});
        }
        return close;
    }

    void part_editor_model::apply_completion(std::size_t index, const parameter_completion &completion)
    {
        if (index >= m_doc.part.parameters.size())
            return;
        document next = m_doc;
        library::parameter &p = next.part.parameters[index];
        p.name = completion.name;
        if (trim(p.unit).empty())
            p.unit = completion.unit;
        edit("Rename parameter", std::move(next));
    }

    std::vector<library::issue> part_editor_model::parameter_issues(std::size_t index) const
    {
        std::vector<library::issue> found;
        if (index >= m_doc.part.parameters.size())
            return found;
        const std::string_view name = trim(m_doc.part.parameters[index].name);
        if (name.empty())
            return found;
        for (const library::issue &i : m_issues)
            if (i.parameter == name)
                found.push_back(i);
        return found;
    }

    // ---- the device -----------------------------------------------------------------------------

    std::vector<device_choice> part_editor_model::device_choices() const
    {
        std::vector<device_choice> choices{{"", "None"}};
        for (const library::device_definition &d : m_schema->devices())
            choices.push_back({d.id, d.name.empty() ? d.id : d.name});
        if (!trim(m_doc.part.device).empty() && m_schema->find_device(m_doc.part.device) == nullptr)
            choices.push_back({m_doc.part.device, m_doc.part.device + " (not a device Volt knows)"});
        return choices;
    }

    std::size_t part_editor_model::device_index() const
    {
        if (trim(m_doc.part.device).empty())
            return 0;
        const library::device_definition *known = m_schema->find_device(m_doc.part.device);
        const std::vector<device_choice> choices = device_choices();
        for (std::size_t i = 1; i < choices.size(); ++i)
        {
            if (known != nullptr ? choices[i].id == known->id : choices[i].id == m_doc.part.device)
                return i;
        }
        return 0;
    }

    std::vector<library::device_parameter> part_editor_model::missing_parameters(bool required_only) const
    {
        std::vector<library::device_parameter> missing;
        const library::device_definition *d = m_schema->find_device(m_doc.part.device);
        if (d == nullptr)
            return missing;
        for (const library::device_parameter &q : d->parameters)
        {
            if (required_only && !q.required)
                continue;
            if (m_schema->find_parameter(m_doc.part, q.name) == nullptr)
                missing.push_back(q);
        }
        return missing;
    }

    std::size_t part_editor_model::add_device_parameters(bool required_only)
    {
        // All of them one step: one click added them.
        const std::vector<library::device_parameter> missing = missing_parameters(required_only);
        document next = m_doc;
        for (const library::device_parameter &q : missing)
            add_known(next.part, q.name);
        edit(missing.size() == 1 ? "Add device parameter" : "Add device parameters", std::move(next));
        return missing.size();
    }

    // ---- checks ---------------------------------------------------------------------------------

    std::vector<std::string> part_editor_model::save_problems() const
    {
        std::vector<std::string> problems;
        // What lists show it by: its id says nothing about it.
        if (trim(m_doc.part.name).empty() && trim(m_doc.part.mpn).empty())
            problems.push_back("Give the part a name or an MPN.");
        for (std::size_t i = 0; i < m_doc.part.parameters.size(); ++i)
        {
            const library::parameter &p = m_doc.part.parameters[i];
            if (trim(p.name).empty() && !blank(p))
                problems.push_back(std::format("Parameter {} has a value but no name.", i + 1));
        }
        if (!m_doc.model_file)
        {
            if (const auto file = typed_model_file(m_doc.part.model))
            {
                std::error_code error;
                if (!std::filesystem::is_regular_file(*file, error))
                    problems.push_back(std::format("The 3D model file {} is not there.", trim(m_doc.part.model)));
            }
        }
        return problems;
    }

    // ---- the 3D model ---------------------------------------------------------------------------

    std::expected<void, std::string> part_editor_model::attach_model(const std::filesystem::path &file)
    {
        auto read = library::read_file(file);
        if (!read)
            return std::unexpected(std::move(read.error()));
        document next = m_doc;
        next.part.model = read->name;
        // The same file again is no change, and no step.
        if (!m_doc.model_file || **m_doc.model_file != *read)
            next.model_file = std::make_shared<const std::optional<library::embedded_file>>(std::move(*read));
        edit("Attach model", std::move(next));
        return {};
    }

    const std::optional<library::embedded_file> &part_editor_model::attached_model() const noexcept
    {
        static const std::optional<library::embedded_file> none;
        return m_doc.model_file ? *m_doc.model_file : none;
    }

    // ---- saving ---------------------------------------------------------------------------------

    std::expected<std::string, std::string> part_editor_model::commit(library::library &target)
    {
        if (const std::vector<std::string> problems = save_problems(); !problems.empty())
            return std::unexpected(problems.front());

        library::part p = cleaned();

        // What replace_part would refuse, before the target is touched:
        // then nothing below can leave it half changed. A new part has no
        // id for add_part to refuse: it is given one there.
        if (m_mode == editor_mode::edit && target.find(m_original_id) == nullptr)
            return std::unexpected(std::format("{} has no part \xE2\x80\x9C{}\xE2\x80\x9D any more",
                                               target.display_name(), m_original_id));

        // The model file goes into the library with the part.
        std::optional<library::embedded_file> file = attached_model();
        if (!file)
        {
            if (const auto typed = typed_model_file(p.model))
            {
                auto read = library::read_file(*typed);
                if (!read)
                    return std::unexpected(std::move(read.error()));
                file = std::move(*read);
            }
        }
        // add_file and carry_files only append, so this is what undoes them.
        const std::size_t files_before = target.files.size();
        if (file)
            p.model = library::add_file(target, std::move(*file));
        if (m_mode == editor_mode::duplicate)
            library::carry_files(target, m_carried, p);

        std::string id;
        if (m_mode == editor_mode::edit)
        {
            id = m_original_id;
            // The model it had: its file goes from the library once nothing
            // names it (given another model, or none).
            const std::string old_model = target.find(m_original_id)->model;
            if (auto replaced = library::replace_part(target, m_original_id, std::move(p)); !replaced)
            {
                target.files.resize(files_before);
                return std::unexpected(std::move(replaced.error()));
            }
            remove_file_if_unused(target, old_model);
        }
        else
        {
            auto added = library::add_part(target, std::move(p), library::on_conflict::fail);
            if (!added)
            {
                target.files.resize(files_before);
                return std::unexpected(std::move(added.error()));
            }
            id = added->id;
        }

        // From now on it is that part of that library, as saved: what the
        // library made of it (an id, parameters trimmed, the model file's
        // name there), the file now in the library rather than attached.
        const library::part *stored = target.find(id);
        const document saved{stored != nullptr ? *stored : m_doc.part, nullptr};
        m_mode = editor_mode::edit;
        m_original_id = saved.part.id;
        m_carried.clear();

        // The history stays, so that undo goes back past the save. The
        // state committed is the saved one now: undone and redone, the part
        // is as saved again, and unmodified. Every state has the part's id,
        // which nothing undoes. A step the library made into nothing (a
        // blank row added, which the save left out) goes (rewrite drops it).
        // Each step holds one value change (they merge), from the state at
        // its index to the next; the ends are fixed apart in case of more.
        const std::size_t at = m_history.position();
        m_history.rewrite([&](std::size_t index, std::span<change> changes) {
            for (change &c : changes)
            {
                c.before.part.id = saved.part.id;
                c.after.part.id = saved.part.id;
            }
            if (index == at)
                changes.front().before = saved;
            if (index + 1 == at)
                changes.back().after = saved;
        });
        m_history.mark_saved();
        m_doc = saved;
        m_baseline = saved;
        changed();
        return id;
    }

} // namespace volt::ui
