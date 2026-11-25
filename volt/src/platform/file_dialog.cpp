#include <platform/file_dialog.hpp>

#include <algorithm>
#include <system_error>

namespace platform
{
    namespace
    {
        char lower(char c)
        {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        }

        char upper(char c)
        {
            return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
        }

        // zenity and kdialog match patterns as the file system spells
        // names, which on Linux is case by case: "*.step" misses
        // "BRACKET.STEP". So each extension goes in twice, as written in
        // lower case and in upper (mixed case is rare enough to leave out).
        std::string chooser_patterns(const file_filter &filter)
        {
            std::string patterns;
            for (const std::string &extension : filter_extensions(filter))
            {
                if (!patterns.empty())
                    patterns += ' ';
                if (extension == "*")
                {
                    patterns += '*';
                    continue;
                }
                patterns += "*." + extension;
                std::string shouted = extension;
                std::ranges::transform(shouted, shouted.begin(), upper);
                if (shouted != extension)
                    patterns += " *." + shouted;
            }
            return patterns;
        }

        // Where the dialog starts, as zenity and kdialog take it: forward
        // slashes (which is all Linux has; the tests build Linux's command
        // lines on Windows too), the suggested name appended when saving.
        std::string start_path(const file_dialog &dialog, bool folder_slash)
        {
            std::string start;
            if (!dialog.directory.empty())
            {
                const std::u8string text = dialog.directory.generic_u8string();
                start.assign(text.begin(), text.end());
                if (!start.ends_with('/'))
                    start += '/';
            }
            if (dialog.mode == file_dialog_mode::save_file && !dialog.file_name.empty())
                start += dialog.file_name;
            else if (!folder_slash && start.size() > 1 && start.ends_with('/'))
                start.pop_back();
            return start;
        }
    } // namespace

    std::string normalize_extension(std::string_view extension)
    {
        if (extension.starts_with("*."))
            extension.remove_prefix(2);
        else if (extension.starts_with('.'))
            extension.remove_prefix(1);

        if (extension == "*")
            return "*";
        if (extension.empty())
            return {};

        std::string result;
        result.reserve(extension.size());
        for (const char c : extension)
        {
            const auto byte = static_cast<unsigned char>(c);
            if (byte < 0x20 || byte == 0x7F)
                return {};
            switch (c)
            {
            case '*':
            case '?':
            case '.':
            case '/':
            case '\\':
            case ';':
            case ' ':
            case '|':
            case '(':
            case ')':
            case '[':
            case ']':
            case '"':
                return {};
            default:
                result += lower(c);
            }
        }
        return result;
    }

    std::vector<std::string> filter_extensions(const file_filter &filter)
    {
        std::vector<std::string> result;
        for (const std::string &extension : filter.extensions)
        {
            std::string normal = normalize_extension(extension);
            if (!normal.empty() && std::ranges::find(result, normal) == result.end())
                result.push_back(std::move(normal));
        }
        return result;
    }

    std::string filter_patterns(const file_filter &filter, std::string_view separator)
    {
        std::string patterns;
        for (const std::string &extension : filter_extensions(filter))
        {
            if (!patterns.empty())
                patterns += separator;
            patterns += extension == "*" ? std::string("*") : "*." + extension;
        }
        return patterns;
    }

    std::string filter_label(const file_filter &filter, std::string_view separator)
    {
        if (filter.name.ends_with(')'))
            return filter.name;
        const std::string patterns = filter_patterns(filter, separator);
        if (filter.name.empty())
            return patterns;
        if (patterns.empty())
            return filter.name;
        return filter.name + " (" + patterns + ")";
    }

    bool filter_matches(const file_filter &filter, const std::filesystem::path &path)
    {
        const std::vector<std::string> extensions = filter_extensions(filter);
        if (std::ranges::find(extensions, "*") != extensions.end())
            return true;
        const std::string extension = normalize_extension(path_to_utf8(path.extension()));
        return !extension.empty() && std::ranges::find(extensions, extension) != extensions.end();
    }

    std::filesystem::path with_default_extension(const std::filesystem::path &path, std::string_view extension)
    {
        if (extension.starts_with('.'))
            extension.remove_prefix(1);
        if (extension.empty() || !path.has_filename())
            return path;

        // "Blinky." has the extension "." (and so a file name ending in
        // the dot); "Blinky" and ".voltrc" have none.
        const std::filesystem::path current = path.extension();
        if (!current.empty() && current != ".")
            return path;

        std::filesystem::path result = path;
        result += path_from_utf8(current.empty() ? "." + std::string(extension) : std::string(extension));
        return result;
    }

    std::filesystem::path path_from_utf8(std::string_view text)
    {
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    }

    std::string path_to_utf8(const std::filesystem::path &path)
    {
        const std::u8string text = path.u8string();
        return std::string(text.begin(), text.end());
    }

    std::vector<std::string> chooser_command(chooser_program program, const file_dialog &dialog)
    {
        std::vector<std::string> command;
        const bool folder = dialog.mode == file_dialog_mode::choose_folder;

        if (program == chooser_program::zenity)
        {
            command = {"zenity", "--file-selection"};
            if (!dialog.title.empty())
                command.push_back("--title=" + dialog.title);
            switch (dialog.mode)
            {
            case file_dialog_mode::open_file:
                break;
            case file_dialog_mode::open_files:
                command.push_back("--multiple");
                command.push_back("--separator=\n");
                break;
            case file_dialog_mode::save_file:
                // Newer zenity always asks before replacing a file and
                // takes the option for a no-op; older ones need it.
                command.push_back("--save");
                command.push_back("--confirm-overwrite");
                break;
            case file_dialog_mode::choose_folder:
                command.push_back("--directory");
                break;
            }
            // A trailing slash makes it the folder to start in.
            if (const std::string start = start_path(dialog, true); !start.empty())
                command.push_back("--filename=" + start);
            if (!folder)
            {
                for (const file_filter &filter : dialog.filters)
                {
                    const std::string patterns = chooser_patterns(filter);
                    if (patterns.empty())
                        continue;
                    // "NAME | PATTERNS": a bar in the name would end it.
                    std::string label = filter_label(filter, " ");
                    std::ranges::replace(label, '|', '/');
                    command.push_back("--file-filter=" + label + " | " + patterns);
                }
            }
            return command;
        }

        // kdialog: options first, then the dialog and its arguments. The
        // filter is KDE's "PATTERNS|NAME", one per line. Without a folder,
        // ":volt" starts where the last kdialog that said ":volt" ended.
        command = {"kdialog"};
        if (!dialog.title.empty())
        {
            command.push_back("--title");
            command.push_back(dialog.title);
        }
        std::string start = start_path(dialog, false);
        if (start.empty())
            start = ":volt";
        switch (dialog.mode)
        {
        case file_dialog_mode::open_file:
            command.push_back("--getopenfilename");
            break;
        case file_dialog_mode::open_files:
            command.push_back("--multiple");
            command.push_back("--separate-output");
            command.push_back("--getopenfilename");
            break;
        case file_dialog_mode::save_file:
            command.push_back("--getsavefilename");
            break;
        case file_dialog_mode::choose_folder:
            command.push_back("--getexistingdirectory");
            break;
        }
        command.push_back(start);
        if (!folder)
        {
            std::string filters;
            for (const file_filter &filter : dialog.filters)
            {
                const std::string patterns = chooser_patterns(filter);
                if (patterns.empty())
                    continue;
                if (!filters.empty())
                    filters += '\n';
                std::string label = filter_label(filter, " ");
                std::ranges::replace(label, '\n', ' ');
                filters += patterns + "|" + label;
            }
            if (!filters.empty())
                command.push_back(filters);
        }
        return command;
    }

    std::vector<std::filesystem::path> parse_chooser_output(std::string_view output, const file_dialog &dialog)
    {
        std::vector<std::filesystem::path> paths;
        while (!output.empty())
        {
            const std::size_t end = output.find('\n');
            std::string_view line = output.substr(0, end);
            output = end == std::string_view::npos ? std::string_view() : output.substr(end + 1);
            if (line.ends_with('\r'))
                line.remove_suffix(1);
            if (line.empty())
                continue;

            std::filesystem::path path = path_from_utf8(line);
            if (dialog.mode == file_dialog_mode::save_file)
                path = with_default_extension(path, dialog.default_extension);
            paths.push_back(std::move(path));
        }
        return paths;
    }

    std::filesystem::path unconfirmed_replacement(std::string_view output, const file_dialog &dialog)
    {
        if (dialog.mode != file_dialog_mode::save_file)
            return {};
        file_dialog as_typed = dialog;
        as_typed.default_extension.clear();
        const std::vector<std::filesystem::path> typed = parse_chooser_output(output, as_typed);
        if (typed.empty())
            return {};
        const std::filesystem::path completed = with_default_extension(typed.front(), dialog.default_extension);
        std::error_code error;
        if (completed == typed.front() || !std::filesystem::exists(completed, error))
            return {};
        return completed;
    }

} // namespace platform
