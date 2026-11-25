// The JSON reader and writer: what they accept and refuse, where they say
// the problem is, how they write, and that what they write reads back as
// the same value.

#include "../math/check.hpp"

#include <json/json.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>

using namespace volt;

namespace
{
    bool parses(std::string_view text)
    {
        return json::parse(text).has_value();
    }

    json::value value_of(std::string_view text)
    {
        auto parsed = json::parse(text);
        if (!parsed)
        {
            std::fprintf(stderr, "  unexpected parse error for %.*s: %s\n", static_cast<int>(text.size()), text.data(),
                         parsed.error().message().c_str());
            return {};
        }
        return std::move(*parsed);
    }

    std::string string_of(std::string_view text)
    {
        return std::string(value_of(text).string_or("<not a string>"));
    }

    void scalars()
    {
        CHECK(value_of("null").is_null());
        CHECK(value_of("true").as_bool() == true);
        CHECK(value_of("false").as_bool() == false);
        CHECK(value_of(" \t\r\n 42 \n").as_integer() == 42);
        CHECK(value_of("\"x\"").as_string() == "x");

        CHECK(!parses("nul"));
        CHECK(!parses("True"));
        CHECK(!parses("nullx"));
        CHECK(!parses("undefined"));
    }

    void numbers()
    {
        CHECK(value_of("0").as_number() == 0.0);
        CHECK(value_of("-0").as_number() == 0.0);
        CHECK(std::signbit(*value_of("-0").as_number()));
        CHECK(value_of("1.5").as_number() == 1.5);
        CHECK(value_of("-12.25").as_number() == -12.25);
        CHECK(value_of("1e3").as_number() == 1000.0);
        CHECK(value_of("1E+3").as_number() == 1000.0);
        CHECK(value_of("25e-2").as_number() == 0.25);
        CHECK(value_of("0.1").as_number() == 0.1);
        CHECK(value_of("1.7976931348623157e308").as_number() == std::numeric_limits<double>::max());
        CHECK(value_of("5e-324").as_number() == std::numeric_limits<double>::denorm_min());

        // Integers: whole and within 2^53.
        CHECK(value_of("123456789012345").as_integer() == 123456789012345);
        CHECK(value_of("-9007199254740992").as_integer() == -9007199254740992LL);
        CHECK(value_of("9007199254740992").is_integer());
        CHECK(!value_of("18014398509481984").is_integer()); // 2^54
        CHECK(!value_of("1.5").as_integer());
        CHECK(value_of("1.0").as_integer() == 1);
        CHECK(value_of("1e2").as_integer() == 100);

        // Too small is zero, too large is an error.
        CHECK(value_of("1e-400").as_number() == 0.0);
        CHECK(std::signbit(*value_of("-1e-400").as_number()));
        CHECK(value_of("0.0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
                       "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
                       "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
                       "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000001")
                  .as_number() == 0.0);
        CHECK(!parses("1e400"));
        CHECK(!parses("-1e400"));
        CHECK(json::parse("1e400").error().reason.find("too large") != std::string::npos);

        // Not JSON numbers.
        for (const char *bad : {"01", "-", "1.", ".5", "+1", "1e", "1e+", "0x10", "NaN", "Infinity", "-Infinity",
                                "1.e5", "- 1", "00", "-01", "1e1.5", "1_000"})
        {
            if (parses(bad))
                std::fprintf(stderr, "  accepted %s\n", bad);
            CHECK(!parses(bad));
        }
    }

    void strings()
    {
        CHECK(string_of(R"("a\"b\\c\/d")") == "a\"b\\c/d");
        CHECK(string_of(R"("\b\f\n\r\t")") == "\b\f\n\r\t");
        CHECK(string_of(R"("\u0041\u00e9\u20AC")") == "A\xC3\xA9\xE2\x82\xAC");
        CHECK(string_of(R"("\uD83D\uDE00")") == "\xF0\x9F\x98\x80"); // a surrogate pair
        CHECK(string_of(R"("\ud83d\ude00")") == "\xF0\x9F\x98\x80");
        CHECK(string_of(R"("\u0000")") == std::string(1, '\0'));
        CHECK(string_of("\"caf\xC3\xA9 \xE2\x84\xA6 \xF0\x9F\x98\x80\"") == "caf\xC3\xA9 \xE2\x84\xA6 \xF0\x9F\x98\x80");
        CHECK(string_of("\"\"").empty());

        CHECK(!parses(R"("\uD83D")"));        // a high surrogate alone
        CHECK(!parses(R"("\uD83Dx")"));       // followed by something else
        CHECK(!parses(R"("\uD83D\u0041")"));  // followed by a non-surrogate escape
        CHECK(!parses(R"("\uDE00")"));        // a low surrogate alone
        CHECK(!parses(R"("\uDE00\uD83D")"));  // the wrong way round
        CHECK(!parses(R"("\x41")"));          // not an escape JSON has
        CHECK(!parses(R"("\u12")"));          // too short
        CHECK(!parses(R"("\u12G4")"));        // not hex
        CHECK(!parses(R"("\')"));
        CHECK(!parses("\"a\nb\""));           // a raw control character
        CHECK(!parses("\"a\tb\""));
        CHECK(!parses("\"abc"));              // not closed
        CHECK(!parses("\"abc\\"));
        CHECK(!parses("'abc'"));              // single quotes

        // Invalid UTF-8: a bad continuation, an overlong form, an encoded
        // surrogate, past U+10FFFF, cut short, a stray continuation byte.
        for (const char *bad : {"\"\xC3\x28\"", "\"\xC0\xAF\"", "\"\xE0\x80\xAF\"", "\"\xED\xA0\x80\"",
                                "\"\xF4\x90\x80\x80\"", "\"\xE2\x82\"", "\"\x80\"", "\"\xFF\""})
            CHECK(!parses(bad));
        CHECK(json::parse("\"\xC3\x28\"").error().reason.find("UTF-8") != std::string::npos);
    }

    void structures()
    {
        CHECK(value_of("[]").as_array()->empty());
        CHECK(value_of("{}").as_object()->empty());
        CHECK(value_of("[ ]").is_array());
        CHECK(value_of("{ \n }").is_object());

        const json::value doc = value_of(R"({"b": 1, "a": [true, null, {"x": "y"}], "c": {"d": {}}})");
        CHECK(doc.size() == 3);
        CHECK(doc["a"][2]["x"].string_or() == "y");
        CHECK(doc["a"][1].is_null());
        CHECK(doc["c"]["d"].is_object());

        // The order of the keys is kept.
        const json::object &o = *doc.as_object();
        auto it = o.begin();
        CHECK(it->key == "b");
        ++it;
        CHECK(it->key == "a");
        ++it;
        CHECK(it->key == "c");

        // Duplicate keys: refused, unless asked for, when the last wins in
        // the place of the first.
        CHECK(!parses(R"({"a": 1, "a": 2})"));
        CHECK(json::parse(R"({"a": 1, "a": 2})").error().reason.find("twice") != std::string::npos);
        json::parse_options lenient;
        lenient.allow_duplicate_keys = true;
        const auto dup = json::parse(R"({"a": 1, "b": 0, "a": 2})", lenient);
        CHECK(dup.has_value());
        if (dup)
        {
            CHECK(dup->at("a").as_integer() == 2);
            CHECK(dup->as_object()->begin()->key == "a");
            CHECK(dup->size() == 2);
        }

        for (const char *bad : {"[1,]", "{\"a\":1,}", "[1 2]", "{\"a\" 1}", "{\"a\":}", "{a:1}", "{1:1}", "[",
                                "{", "[1", "{\"a\":1", "]", "}", "[1]]", "1 2", "", "   ", "// x\n1", "/* x */ 1",
                                "[1,,2]", "[,1]", "{,}", "{\"a\":1 \"b\":2}"})
        {
            if (parses(bad))
                std::fprintf(stderr, "  accepted '%s'\n", bad);
            CHECK(!parses(bad));
        }

        // A UTF-8 byte order mark is skipped.
        CHECK(value_of("\xEF\xBB\xBF{\"a\": 1}")["a"].as_integer() == 1);
    }

    void error_positions()
    {
        const auto error = json::parse("{\n  \"a\": 1,\n  \"b\": tru\n}");
        CHECK(!error.has_value());
        if (!error)
        {
            CHECK(error.error().line == 3);
            CHECK(error.error().column == 8);
            CHECK(error.error().message().starts_with("line 3, column 8: "));
        }

        // Columns count characters, not bytes.
        const auto wide = json::parse("[\"\xC3\xA9\xC3\xA9\", x]");
        CHECK(!wide.has_value());
        if (!wide)
        {
            CHECK(wide.error().line == 1);
            CHECK(wide.error().column == 8);
        }

        // An unclosed string is reported where it opens.
        const auto open = json::parse("[1,\n \"abc");
        CHECK(!open.has_value());
        if (!open)
        {
            CHECK(open.error().line == 2);
            CHECK(open.error().column == 2);
        }

        // A duplicate key is reported at the second one.
        const auto dup = json::parse("{\"a\": 1,\n\"a\": 2}");
        CHECK(!dup.has_value());
        if (!dup)
            CHECK(dup.error().line == 2 && dup.error().column == 1);

        // After the byte order mark, columns are as an editor shows them.
        const auto bom = json::parse("\xEF\xBB\xBFx");
        CHECK(!bom.has_value());
        if (!bom)
            CHECK(bom.error().column == 1);
    }

    void depth_limit()
    {
        const auto nested = [](std::size_t depth) { return std::string(depth, '[') + std::string(depth, ']'); };
        CHECK(parses(nested(256)));
        CHECK(!parses(nested(257)));
        CHECK(json::parse(nested(257)).error().reason.find("deeper") != std::string::npos);

        json::parse_options shallow;
        shallow.max_depth = 3;
        CHECK(json::parse("[[[1]]]", shallow).has_value());
        CHECK(!json::parse("[[[[1]]]]", shallow).has_value());
        CHECK(!json::parse(R"({"a":{"b":{"c":{"d":1}}}})", shallow).has_value());

        // Far deeper than the stack would take is refused, not a crash.
        CHECK(!parses(std::string(1'000'000, '[')));
    }

    void value_access()
    {
        json::value v;
        CHECK(v.is_null());
        CHECK(v.size() == 0);
        CHECK(v["missing"]["deeper"].is_null());
        CHECK(v.at(5).is_null());
        CHECK(v.find("x") == nullptr);

        v["name"] = "R1";
        v["pins"] = 2;
        v["ratio"] = 0.5;
        v["on"] = true;
        v["tags"].push_back("a");
        v["tags"].push_back("b");
        CHECK(v.is_object());
        CHECK(v["name"].string_or() == "R1");
        CHECK(v["pins"].as_integer() == 2);
        CHECK(v["ratio"].number_or(0.0) == 0.5);
        CHECK(v["on"].bool_or(false));
        CHECK(v["tags"].size() == 2);
        CHECK(v["tags"][1].string_or() == "b");

        // Kinds are not converted.
        CHECK(!v["pins"].as_string());
        CHECK(!v["name"].as_number());
        CHECK(v["name"].integer_or(-1) == -1);
        CHECK(v["pins"].string_or("fallback") == "fallback");
        CHECK(!v["on"].as_integer());
        CHECK(v["name"].as_array() == nullptr);
        CHECK(v["tags"].as_object() == nullptr);

        // An existing key keeps its place when set again.
        json::object o{{"a", 1}, {"b", 2}, {"c", 3}};
        o.set("b", "two");
        CHECK(o.size() == 3);
        CHECK((o.begin() + 1)->key == "b");
        CHECK(o.find("b")->string_or() == "two");
        CHECK(o.erase("a"));
        CHECK(!o.erase("a"));
        CHECK(o.begin()->key == "b");
        CHECK(!o.contains("a"));
        o["d"] = nullptr;
        CHECK(o.contains("d"));

        // Duplicates in an initializer list: the first place, the last value.
        const json::object twice{{"k", 1}, {"j", 0}, {"k", 2}};
        CHECK(twice.size() == 2);
        CHECK(twice.begin()->key == "k" && twice.begin()->value.as_integer() == 2);

        // Equality is JSON's: objects in any order, arrays in theirs.
        CHECK(value_of(R"({"a":1,"b":[1,2]})") == value_of(R"({"b":[1,2],"a":1})"));
        CHECK(!(value_of(R"({"a":1,"b":[1,2]})") == value_of(R"({"a":1,"b":[2,1]})")));
        CHECK(!(value_of(R"({"a":1})") == value_of(R"({"a":1,"b":2})")));
        CHECK(!(value_of("1") == value_of("\"1\"")));
        CHECK(value_of("1") == json::value(1.0));

        // Typing helpers.
        CHECK(json::value('x' == 'x').is_bool());
        CHECK(json::value(std::int64_t{7}).as_integer() == 7);
        CHECK(json::value(3.0f).as_number() == 3.0);
        CHECK(json::describe(json::kind::object) == "an object");
    }

    void writing()
    {
        json::value v = json::object{{"name", "R1"},
                                     {"pins", 2},
                                     {"tags", json::array{"a", "b"}},
                                     {"empty", json::array{}},
                                     {"none", json::object{}},
                                     {"nested", json::object{{"x", 1.5}, {"y", nullptr}, {"z", false}}}};
        CHECK(json::write_compact(v) ==
              R"({"name":"R1","pins":2,"tags":["a","b"],"empty":[],"none":{},"nested":{"x":1.5,"y":null,"z":false}})");

        // Pretty: a container of scalars that fits stays on one line.
        const std::string pretty = json::write(v);
        CHECK(pretty == "{\n"
                        "  \"name\": \"R1\",\n"
                        "  \"pins\": 2,\n"
                        "  \"tags\": [\"a\", \"b\"],\n"
                        "  \"empty\": [],\n"
                        "  \"none\": {},\n"
                        "  \"nested\": {\"x\": 1.5, \"y\": null, \"z\": false}\n"
                        "}\n");

        // Without the inline rule, or when it does not fit, one per line.
        json::write_options tall;
        tall.line_width = 0;
        tall.final_newline = false;
        CHECK(json::write(json::array{1, 2}, tall) == "[\n  1,\n  2\n]");
        json::array long_array;
        for (int i = 0; i < 40; ++i)
            long_array.emplace_back(1000 + i);
        const std::string wrapped = json::write(json::value(long_array));
        CHECK(wrapped.starts_with("[\n  1000,\n  1001,"));

        // A container holding containers is never inline.
        CHECK(json::write(json::array{json::array{1}}) == "[\n  [1]\n]\n");

        json::write_options four;
        four.indent = 4;
        CHECK(json::write(json::object{{"a", json::array{json::object{{"b", 1}}}}}, four) ==
              "{\n    \"a\": [\n        {\"b\": 1}\n    ]\n}\n");

        // Scalars at the top.
        CHECK(json::write(json::value(1)) == "1\n");
        CHECK(json::write_compact(json::value()) == "null");
        CHECK(json::write_compact(json::array{}) == "[]");
    }

    void writing_strings_and_numbers()
    {
        CHECK(json::write_compact("a\"b\\c/d") == R"("a\"b\\c/d")");
        CHECK(json::write_compact("\b\f\n\r\t") == R"("\b\f\n\r\t")");
        CHECK(json::write_compact(std::string("\x01\x1f\x7f", 3)) == "\"\\u0001\\u001f\x7f\"");
        CHECK(json::write_compact(std::string(1, '\0')) == R"("\u0000")");
        CHECK(json::write_compact("caf\xC3\xA9 \xF0\x9F\x98\x80") == "\"caf\xC3\xA9 \xF0\x9F\x98\x80\"");

        json::write_options ascii;
        ascii.pretty = false;
        ascii.ascii = true;
        CHECK(json::write("caf\xC3\xA9 \xE2\x84\xA6 \xF0\x9F\x98\x80", ascii) == R"("caf\u00e9 \u2126 \ud83d\ude00")");

        // Invalid UTF-8 comes out as U+FFFD, so the output is valid JSON.
        const std::string replaced = json::write_compact(std::string("a\xFF" "b\xC3", 4));
        CHECK(replaced == "\"a\xEF\xBF\xBD" "b\xEF\xBF\xBD\"");
        CHECK(parses(replaced));

        CHECK(json::write_compact(0.1) == "0.1");
        CHECK(json::write_compact(100.0) == "100");
        CHECK(json::write_compact(-0.0) == "0");
        CHECK(json::write_compact(1.5) == "1.5");
        CHECK(json::write_compact(-2.25e-7) == "-2.25e-07");
        CHECK(json::write_compact(1e21) == "1e+21");
        CHECK(json::write_compact(9007199254740992.0) == "9007199254740992");
        CHECK(json::write_compact(123456789.125) == "123456789.125");
        CHECK(json::write_compact(std::numeric_limits<double>::quiet_NaN()) == "null");
        CHECK(json::write_compact(std::numeric_limits<double>::infinity()) == "null");
        CHECK(json::write_compact(-std::numeric_limits<double>::infinity()) == "null");
    }

    void round_trips()
    {
        // Doubles come back bit for bit.
        const double samples[] = {0.1 + 0.2, 1.0 / 3.0, 2.0 / 3.0, 1e-300, 4.9e-324, 2.2250738585072014e-308,
                                  1.7976931348623157e308, 123456.789e10, -98765.4321, 3.141592653589793,
                                  9007199254740993.0, 1e15 + 0.5, 4.7e-6, 1e-7};
        for (const double d : samples)
        {
            const auto back = json::parse(json::write_compact(d));
            CHECK(back.has_value() && back->as_number() == d);
        }

        const json::value doc = value_of(
            R"({"library": "volt", "parts": [{"id": "R_10k", "value": "10k", "params": [{"name": "Resistance",)"
            R"( "value": "10k", "unit": "\u03a9"}], "tags": ["smd", "chip"], "x": -1.25e-3, "ok": true,)"
            R"( "nothing": null, "quote": "a\"b", "emoji": "\ud83d\ude00", "deep": [[[[{}]]]]}], "count": 1})");
        CHECK(doc.is_object());
        for (const bool pretty : {true, false})
        {
            for (const bool ascii : {false, true})
            {
                json::write_options options;
                options.pretty = pretty;
                options.ascii = ascii;
                const std::string text = json::write(doc, options);
                const auto back = json::parse(text);
                CHECK(back.has_value());
                if (!back)
                    continue;
                CHECK(*back == doc);
                // Deterministic: writing what was read writes the same bytes.
                CHECK(json::write(*back, options) == text);
            }
        }
    }

    void files()
    {
        const std::filesystem::path directory = std::filesystem::temp_directory_path() / "volt_json_tests";
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory);

        // A name outside ASCII, which a narrow path on Windows cannot hold.
        const std::filesystem::path path = directory / std::filesystem::path(u8"réglages Ω.json");
        const json::value settings = json::object{{"theme", "dark"}, {"scale", 1.25}, {"recent", json::array{"a", "b"}}};
        CHECK(json::write_file(path, settings).has_value());
        const auto back = json::read_file(path);
        CHECK(back.has_value() && *back == settings);

        // Replacing an existing file, and nothing left beside it.
        const json::value changed = json::object{{"theme", "light"}};
        CHECK(json::write_file(path, changed).has_value());
        CHECK(json::read_file(path).value_or(json::value()) == changed);
        std::size_t files = 0;
        for (const auto &entry : std::filesystem::directory_iterator(directory))
        {
            (void)entry;
            ++files;
        }
        CHECK(files == 1);

        // Errors name the file, and where in it.
        const std::filesystem::path broken = directory / "broken.json";
        CHECK(json::write_text_file(broken, "{\n  \"a\": [1, 2,]\n}\n").has_value());
        const auto bad = json::read_file(broken);
        CHECK(!bad.has_value());
        if (!bad)
        {
            CHECK(bad.error().find("broken.json") != std::string::npos);
            CHECK(bad.error().find("line 2") != std::string::npos);
        }
        const auto missing = json::read_file(directory / "missing.json");
        CHECK(!missing.has_value());
        CHECK(!json::read_text_file(broken, 4).has_value()); // over the size limit

        // Writing into a directory that does not exist fails cleanly.
        CHECK(!json::write_text_file(directory / "no" / "such" / "dir.json", "{}").has_value());

        std::filesystem::remove_all(directory);
    }
} // namespace

int main()
{
    scalars();
    numbers();
    strings();
    structures();
    error_positions();
    depth_limit();
    value_access();
    writing();
    writing_strings_and_numbers();
    round_trips();
    files();
    return volt::tests::result("json");
}
