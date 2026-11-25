#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// -------------------------------------------------------------------------
// json
// -------------------------------------------------------------------------
//
// A strict JSON (RFC 8259) reader and a deterministic writer, for the files
// Volt itself keeps: libraries, projects, settings. Strict because these are
// files people edit by hand and check into git, and a mistake is better
// reported with its line and column than guessed around: no comments, no
// trailing commas, no single quotes, no NaN, no leading zeros, no invalid
// UTF-8, no lone surrogates in \u escapes, and (by default) no key twice in
// one object.
//
//   auto doc = json::parse(R"({"name": "R1", "pins": 2})");
//   doc->at("name").string_or()            "R1"
//   doc->at("pins").as_integer()           2
//   doc->at("missing").string_or("none")   "none": a missing key reads as null
//
//   json::value v = json::object{{"name", "R1"}, {"pins", 2}};
//   v["value"] = "10k";                    appended after "pins"
//   json::write(v)                         pretty, two-space indent
//
// A value is plain data: copyable, comparable, no references to the text it
// came from. Numbers are doubles, as in JavaScript; as_integer() gives the
// ones that are whole and exactly representable. Objects keep their keys in
// the order they were read or inserted, which is what makes the writer's
// output deterministic: the same value always writes the same bytes, so a
// file that is loaded and saved unchanged does not change, and one changed
// field is one changed line in a diff. Key lookup is a linear search, which
// is the right trade for objects of tens of keys; keep large maps as arrays.

namespace volt::json
{

    class value;
    struct member;

    using array = std::vector<value>;

    enum class kind : std::uint8_t
    {
        null,
        boolean,
        number,
        string,
        array,
        object,
    };

    // The name of a kind for messages: "null", "a boolean", "a number", ...
    std::string_view describe(kind k) noexcept;

    // An object: members in order, unique keys.
    class object
    {
    public:
        using iterator = std::vector<member>::iterator;
        using const_iterator = std::vector<member>::const_iterator;

        // Declared here and defaulted below, where `member` is complete:
        // the standard library may not instantiate vector<member>'s members
        // before then.
        object() noexcept;
        object(const object &other);
        object(object &&other) noexcept;
        object &operator=(const object &other);
        object &operator=(object &&other) noexcept;
        ~object();

        // A key given twice keeps its first position and its last value.
        object(std::initializer_list<member> members);

        std::size_t size() const noexcept;
        bool empty() const noexcept;
        void reserve(std::size_t count);
        void clear() noexcept;

        iterator begin() noexcept;
        iterator end() noexcept;
        const_iterator begin() const noexcept;
        const_iterator end() const noexcept;

        // The member's value, or nullptr.
        const value *find(std::string_view key) const noexcept;
        value *find(std::string_view key) noexcept;
        bool contains(std::string_view key) const noexcept;

        // Sets a member. An existing key keeps its position, a new one goes
        // at the end. Returns the stored value.
        value &set(std::string_view key, value v);

        // The member's value, inserted as null at the end if missing.
        value &operator[](std::string_view key);

        // Removes the member; false if there was none.
        bool erase(std::string_view key);

        // JSON equality: the same keys with equal values, in any order.
        bool operator==(const object &other) const;

    private:
        std::vector<member> m_members;
    };

    class value
    {
    public:
        value() noexcept = default; // null
        value(std::nullptr_t) noexcept {}
        value(bool b) noexcept : m_data(std::in_place_index<1>, b) {}
        template <std::integral I>
            requires(!std::same_as<I, bool> && !std::same_as<I, char>)
        value(I i) noexcept : m_data(std::in_place_index<2>, static_cast<double>(i))
        {
        }
        template <std::floating_point F>
        value(F f) noexcept : m_data(std::in_place_index<2>, static_cast<double>(f))
        {
        }
        // A char would otherwise become a bool; say "x" or 120 instead.
        value(char) = delete;
        value(const char *s) : m_data(std::in_place_index<3>, s) {}
        value(std::string_view s) : m_data(std::in_place_index<3>, s) {}
        value(std::string s) noexcept : m_data(std::in_place_index<3>, std::move(s)) {}
        value(array a) noexcept : m_data(std::in_place_index<4>, std::move(a)) {}
        value(object o) noexcept : m_data(std::in_place_index<5>, std::move(o)) {}

        kind type() const noexcept { return static_cast<kind>(m_data.index()); }

        bool is_null() const noexcept { return type() == kind::null; }
        bool is_bool() const noexcept { return type() == kind::boolean; }
        bool is_number() const noexcept { return type() == kind::number; }
        bool is_string() const noexcept { return type() == kind::string; }
        bool is_array() const noexcept { return type() == kind::array; }
        bool is_object() const noexcept { return type() == kind::object; }

        // A number that is whole and within +-2^53, where every integer is
        // exactly a double.
        bool is_integer() const noexcept;

        // ---- typed access --------------------------------------------------
        //
        // Empty (nullopt, nullptr) when the value is of another kind: a
        // number is never read as a string or the other way round.

        std::optional<bool> as_bool() const noexcept;
        std::optional<double> as_number() const noexcept;
        std::optional<std::int64_t> as_integer() const noexcept;
        std::optional<std::string_view> as_string() const noexcept;
        const array *as_array() const noexcept;
        array *as_array() noexcept;
        const object *as_object() const noexcept;
        object *as_object() noexcept;

        // The same with a fallback, for optional fields.
        bool bool_or(bool fallback) const noexcept;
        double number_or(double fallback) const noexcept;
        std::int64_t integer_or(std::int64_t fallback) const noexcept;
        std::string_view string_or(std::string_view fallback = {}) const noexcept;

        // ---- members and elements ------------------------------------------

        // Number of elements or members; 0 for anything else.
        std::size_t size() const noexcept;

        // The member's value, or nullptr when this is not an object or has
        // no such key.
        const value *find(std::string_view key) const noexcept;
        value *find(std::string_view key) noexcept;

        // The member or element, or a null value when there is none, so
        // that lookups chain without checks: doc.at("a").at("b").string_or().
        const value &at(std::string_view key) const noexcept;
        const value &at(std::size_t index) const noexcept;
        const value &operator[](std::string_view key) const noexcept { return at(key); }
        const value &operator[](std::size_t index) const noexcept { return at(index); }

        // For building: a null value becomes an object (or an array) first.
        // Inserting into a value of another kind replaces it.
        value &operator[](std::string_view key);
        value &operator[](std::size_t index); // precondition: an array, index < size()
        void push_back(value v);

        bool operator==(const value &other) const;

    private:
        // In the order of `kind`, so that index() is the kind.
        std::variant<std::monostate, bool, double, std::string, array, object> m_data;
    };

    struct member
    {
        std::string key;
        json::value value;

        bool operator==(const member &) const = default;
    };

    // ---- reading -----------------------------------------------------------

    struct parse_options
    {
        // Arrays and objects nested deeper than this are an error. Parsing
        // recurses, so this also bounds the stack it uses.
        std::size_t max_depth = 256;

        // RFC 8259 leaves duplicate keys undefined. Rejected by default; when
        // allowed, the last one wins, in the position of the first.
        bool allow_duplicate_keys = false;
    };

    struct parse_error
    {
        // Where the problem is, from 1. The column counts characters (UTF-8
        // code points), not bytes, so it matches what an editor shows.
        std::size_t line = 0;
        std::size_t column = 0;
        std::size_t offset = 0; // in bytes, from 0
        std::string reason;

        // "line 3, column 14: expected ',' or '}'"
        std::string message() const;
    };

    // The whole text as one value, with nothing but whitespace after it. A
    // UTF-8 byte order mark at the start is skipped (Notepad writes one).
    std::expected<value, parse_error> parse(std::string_view text, const parse_options &options = {});

    // ---- writing -----------------------------------------------------------

    struct write_options
    {
        // Pretty: one member or element per line, indented. Otherwise the
        // compact form, with no whitespace at all.
        bool pretty = true;
        int indent = 2;

        // When pretty, an array or object that holds only scalars (and
        // empty containers) is written on one line if that line, indent
        // included, fits in this many characters: tags and small records
        // stay one line each, which reads and diffs better than five. 0
        // puts every element on its own line.
        std::size_t line_width = 100;

        // Escape everything outside ASCII as \u, with surrogate pairs.
        bool ascii = false;

        // End pretty output with a newline, as text files should.
        bool final_newline = true;
    };

    // Deterministic: the same value gives the same bytes. Numbers are the
    // shortest text that reads back as the same double; whole numbers up to
    // 2^53 are written without a fraction or exponent. NaN and infinities,
    // which JSON cannot represent, are written as null. Invalid UTF-8 in a
    // string is written as U+FFFD, so the output is always valid JSON.
    std::string write(const value &v, const write_options &options = {});

    // write(v, {.pretty = false}).
    std::string write_compact(const value &v);

    // ---- files -------------------------------------------------------------

    // The bytes of a file, refusing files larger than limit.
    std::expected<std::string, std::string> read_text_file(const std::filesystem::path &path,
                                                           std::uintmax_t limit = std::uintmax_t{256} << 20);

    // Replaces the file safely: the bytes are written to a temporary file
    // next to it, which is then renamed over it, so a failure (a full disk,
    // a crash) leaves either the old file or the new one, never half of one.
    std::expected<void, std::string> write_text_file(const std::filesystem::path &path, std::string_view bytes);

    // Parses a file; errors name the file and the line and column.
    std::expected<value, std::string> read_file(const std::filesystem::path &path, const parse_options &options = {});

    // Writes a value as write_text_file does.
    std::expected<void, std::string> write_file(const std::filesystem::path &path, const value &v,
                                                const write_options &options = {});

    // ---- inline definitions of object ---------------------------------------
    //
    // Here because they need `member`, which needs `value`, which needs
    // `object`.

    inline object::object() noexcept = default;
    inline object::object(const object &other) = default;
    inline object::object(object &&other) noexcept = default;
    inline object &object::operator=(const object &other) = default;
    inline object &object::operator=(object &&other) noexcept = default;
    inline object::~object() = default;

    inline std::size_t object::size() const noexcept
    {
        return m_members.size();
    }

    inline bool object::empty() const noexcept
    {
        return m_members.empty();
    }

    inline void object::reserve(std::size_t count)
    {
        m_members.reserve(count);
    }

    inline void object::clear() noexcept
    {
        m_members.clear();
    }

    inline object::iterator object::begin() noexcept
    {
        return m_members.begin();
    }

    inline object::iterator object::end() noexcept
    {
        return m_members.end();
    }

    inline object::const_iterator object::begin() const noexcept
    {
        return m_members.begin();
    }

    inline object::const_iterator object::end() const noexcept
    {
        return m_members.end();
    }

    inline const value *object::find(std::string_view key) const noexcept
    {
        for (const member &m : m_members)
            if (m.key == key)
                return &m.value;
        return nullptr;
    }

    inline value *object::find(std::string_view key) noexcept
    {
        for (member &m : m_members)
            if (m.key == key)
                return &m.value;
        return nullptr;
    }

    inline bool object::contains(std::string_view key) const noexcept
    {
        return find(key) != nullptr;
    }

} // namespace volt::json
