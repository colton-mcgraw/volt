#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// -------------------------------------------------------------------------
// part21
// -------------------------------------------------------------------------
//
// ISO 10303-21, the "Part 21" or physical file: the text format every STEP
// file is written in. This is the format only, with no idea what the
// entities mean; <model/step.hpp> interprets them.
//
// A file is a HEADER section of a few entities without names, then one or
// more DATA sections of instances, each `#id = NAME(parameters);`, or for an
// instance of several types at once `#id = (A(...) B(...));`. Parameters are
// numbers, strings, enumerations, references to other instances, lists of
// these and so on (see `kind`).
//
// Real files run to tens of megabytes and millions of instances, so nothing
// here is a tree of heap objects: every parameter is a 16-byte record in one
// array, every string's text sits in one shared buffer, and type names are
// interned. A file in memory is typically two to three times its size on
// disk. `parameter` and `entity` are small views into it, valid as long as
// the file is.
//
// Input is untrusted: parse() answers anything malformed with an error that
// names the line, and limits how deeply lists may nest.

namespace volt::model::part21
{

    enum class kind : std::uint8_t
    {
        omitted,     // $: no value
        derived,     // *: a value the schema computes
        integer,     // 42
        real,        // 1.5, 1.E-07
        string,      // 'text', decoded to UTF-8
        enumeration, // .T. -- held without the dots
        binary,      // "0FF" -- held as written, without the quotes
        reference,   // #12
        list,        // (a, b, c), possibly empty
        typed,       // LENGTH_MEASURE(1.0): a value with its type named
    };

    class file;

    // ---------------------------------------------------------------------
    // parameter
    // ---------------------------------------------------------------------
    //
    // A view of one parameter. Asking a parameter for what it is not is
    // harmless: integer() of a string is 0, an element past the end of a list
    // is `omitted`. A default constructed parameter is `omitted` too.
    class parameter
    {
    public:
        parameter() = default;

        kind type() const noexcept;
        bool is(kind kind) const noexcept { return type() == kind; }

        // Integer or real.
        bool is_number() const noexcept { return is(kind::integer) || is(kind::real); }

        std::int64_t integer() const noexcept;

        // Integer or real, as a double; 0 for anything else.
        double number() const noexcept;

        // The text of a string, enumeration or binary; empty for anything else.
        std::string_view text() const noexcept;

        // The instance name of a reference, 0 for anything else.
        std::uint64_t reference() const noexcept;

        // Elements of a list; 0 for anything else.
        std::size_t size() const noexcept;
        parameter operator[](std::size_t index) const noexcept;

        // The type name and the value of a typed parameter. A typed
        // parameter's value() is itself, unwrapped as far as it goes, for any
        // other parameter.
        std::string_view type_name() const noexcept;
        parameter value() const noexcept;

        // One level down a typed parameter (a typed value may hold another
        // typed value); omitted for anything else.
        parameter inner() const noexcept;

        // An enumeration .T. or .F.; `fallback` for anything else (.U.).
        bool logical(bool fallback) const noexcept;

        // Where it lives in its file. For writers and tests.
        std::uint32_t index() const noexcept { return m_index; }

    private:
        friend class file;
        friend class entity;
        parameter(const file *file, std::uint32_t index) noexcept : m_file(file), m_index(index) {}

        const file *m_file = nullptr;
        std::uint32_t m_index = 0;
    };

    // ---------------------------------------------------------------------
    // entity
    // ---------------------------------------------------------------------
    //
    // A view of one instance, or of one entity of the HEADER (whose id is 0).
    // A simple instance has one part, its type and parameters; a complex
    // instance has a part per type, in the order the file lists them.
    class entity
    {
    public:
        entity() = default;

        bool valid() const noexcept { return m_file != nullptr; }

        std::uint64_t id() const noexcept;
        std::uint32_t line() const noexcept; // where the instance starts in its file
        bool is_complex() const noexcept;

        std::size_t part_count() const noexcept;

        // The type of a part, upper case. For a simple instance, name() is
        // its one type.
        std::string_view name(std::size_t part = 0) const noexcept;
        std::uint32_t name_id(std::size_t part = 0) const noexcept;

        // The parameters of a part, as a list.
        parameter parameters(std::size_t part = 0) const noexcept;

        // The part of the given type, if the instance has one.
        std::optional<std::size_t> find_part(std::string_view type) const noexcept;
        std::optional<std::size_t> find_part(std::uint32_t name_id) const noexcept;

        // Whether the instance is of the type, as its only type or as one of
        // the parts of a complex instance.
        bool is(std::string_view type) const noexcept { return find_part(type).has_value(); }
        bool is(std::uint32_t name_id) const noexcept { return find_part(name_id).has_value(); }

        // Index in the file (file::operator[]); for header entities, index
        // into the header.
        std::uint32_t index() const noexcept { return m_index; }

    private:
        friend class file;
        entity(const file *file, std::uint32_t index, bool header) noexcept
            : m_file(file), m_index(index), m_header(header)
        {
        }

        const file *m_file = nullptr;
        std::uint32_t m_index = 0;
        bool m_header = false;
    };

    // A DATA section: which instances it holds, and the parameters written
    // after DATA in edition 3 files (`DATA('name', ('schema'));`) as a list,
    // or an omitted parameter when there are none.
    struct section
    {
        std::size_t first = 0;
        std::size_t count = 0;
        parameter parameters;
    };

    // ---------------------------------------------------------------------
    // file
    // ---------------------------------------------------------------------

    class file
    {
    public:
        // Instances, in the order of the file.
        std::size_t size() const noexcept { return m_instances.size(); }
        entity operator[](std::size_t index) const noexcept
        {
            return entity(this, static_cast<std::uint32_t>(index), false);
        }

        // The instance with the given name (#id).
        std::optional<std::uint32_t> find(std::uint64_t id) const noexcept;
        entity get(std::uint64_t id) const noexcept;

        std::size_t header_size() const noexcept { return m_header.size(); }
        entity header(std::size_t index) const noexcept { return entity(this, static_cast<std::uint32_t>(index), true); }

        // The header entity of the given type (FILE_NAME, FILE_SCHEMA, ...).
        entity find_header(std::string_view type) const noexcept;

        std::size_t section_count() const noexcept { return m_sections.size(); }
        section get_section(std::size_t index) const noexcept;

        // Interned type names: every entity and typed parameter name in the
        // file has a number. name_id() of a name the file never uses is empty.
        std::optional<std::uint32_t> name_id(std::string_view name) const noexcept;
        std::string_view name(std::uint32_t id) const noexcept;
        std::size_t name_count() const noexcept { return m_names.size(); }

        // Bytes of memory the file holds, roughly.
        std::size_t memory_used() const noexcept;

    private:
        friend class parameter;
        friend class entity;
        friend class builder;

        struct value
        {
            kind type = kind::omitted;
            // Text length, element count, or typed name id.
            std::uint32_t a = 0;
            // Integer, real bits, reference id, text offset, first element
            // index, or the typed value's index.
            std::uint64_t b = 0;
        };

        struct part
        {
            std::uint32_t name = 0;
            std::uint32_t parameters = 0; // index of a list value
        };

        struct instance
        {
            std::uint64_t id = 0;
            std::uint32_t first_part = 0;
            std::uint32_t line = 0;
            std::uint16_t part_count = 0;
            bool complex = false;
        };

        struct section_record
        {
            std::uint32_t first = 0;
            std::uint32_t count = 0;
            std::uint32_t parameters = no_parameters;
        };
        static constexpr std::uint32_t no_parameters = 0xffffffffu;

        struct name_hash
        {
            using is_transparent = void;
            std::size_t operator()(std::string_view name) const noexcept
            {
                return std::hash<std::string_view>{}(name);
            }
        };

        std::vector<value> m_values;
        std::string m_text;
        std::vector<part> m_parts;
        std::vector<instance> m_instances;
        std::vector<instance> m_header;
        std::vector<section_record> m_sections;

        std::vector<std::string> m_names;
        std::unordered_map<std::string, std::uint32_t, name_hash, std::equal_to<>> m_name_index;

        // id -> instance index + 1 when the ids are dense enough (they almost
        // always are), otherwise (id, index) sorted by id.
        std::vector<std::uint32_t> m_dense;
        std::vector<std::pair<std::uint64_t, std::uint32_t>> m_sparse;
    };

    // ---------------------------------------------------------------------
    // builder
    // ---------------------------------------------------------------------
    //
    // Assembles a file the way a parser reads one, value by value. The parser
    // uses it, and so does anything that writes STEP from scratch. Calls must
    // nest properly (every begin_ has its end_); a builder that is misused
    // reports it from finish() instead of building a broken file.
    class builder
    {
    public:
        builder();

        // An entity of the HEADER section: begin_header(name), its
        // parameters, end_entity().
        void begin_header(std::string_view type);

        // Starts a DATA section. An edition 3 section has parameters: call
        // begin_section_parameters(), add them, end_section_parameters(),
        // before the section's first instance.
        void begin_section();
        void begin_section_parameters();
        void end_section_parameters();

        // A simple instance: begin_instance(id), begin_part(type), its
        // parameters, end_part(), end_entity(). A complex instance has
        // several parts and is marked as such.
        void begin_instance(std::uint64_t id, std::uint32_t line = 0, bool complex = false);
        void begin_part(std::string_view type);
        void end_part();
        void end_entity();

        void omitted();
        void derived();
        void integer(std::int64_t value);
        void real(double value);
        void string(std::string_view utf8);
        void enumeration(std::string_view name);
        void binary(std::string_view hex);
        void reference(std::uint64_t id);
        void begin_list();
        void end_list();
        void begin_typed(std::string_view type);
        void end_typed();

        // Nesting depth of lists and typed parameters right now.
        std::size_t depth() const noexcept { return m_frames.size(); }

        // Indexes the instances by id and hands the file over. Fails on a
        // duplicated id or on calls that did not nest properly.
        std::expected<file, std::string> finish();

    private:
        enum class frame_kind : std::uint8_t
        {
            entity,
            part,
            list,
            typed,
            section,
        };

        struct frame
        {
            frame_kind kind;
            std::uint32_t start = 0; // in m_stack
            std::uint32_t name = 0;
        };

        std::uint32_t intern(std::string_view name);
        void push(file::value value);
        std::uint32_t commit(std::uint32_t start);
        void fail(std::string message);

        file m_file;
        std::vector<file::value> m_stack;
        std::vector<frame> m_frames;
        std::vector<file::part> m_pending_parts;
        file::instance m_pending;
        bool m_pending_header = false;
        std::string m_error;
    };

    // ---------------------------------------------------------------------
    // reading and writing
    // ---------------------------------------------------------------------

    // Parses a whole file held in memory.
    std::expected<file, std::string> parse(std::string_view text);

    struct write_options
    {
        // Line break after each instance. Files written on Windows by most
        // tools use "\r\n"; every reader accepts either.
        std::string_view newline = "\n";
    };

    // The file as Part 21 text. Parsing the result gives the same entities:
    // the same instances, types and values, with strings escaped as the
    // standard requires and reals written so that they read back to the same
    // double.
    std::string write(const file &file, const write_options &options = {});

    // A real number as Part 21 writes it: always with a decimal point, so
    // that it is not read back as an integer ("1.", "0.5", "1.E-07").
    std::string format_real(double value);

    // A string's Part 21 form with the quotes, escaping apostrophes,
    // backslashes and everything outside printable ASCII.
    std::string format_string(std::string_view utf8);

} // namespace volt::model::part21
