#include <platform/file_dialog.hpp>
#include <platform/linux/window.hpp>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

// The process's environment, for posix_spawn (unistd.h declares it only
// with _GNU_SOURCE).
extern char **environ;

// zenity or kdialog, as a child process. The desktop portal
// (org.freedesktop.portal.FileChooser) would be the better way, parented
// and sandbox-friendly, but it is spoken over D-Bus, and the backend has no
// D-Bus connection; adding one is a dependency this does not take.

namespace platform::wayland
{

    struct file_chooser
    {
        std::mutex mutex;

        // The running program, -1 when none. Under the mutex.
        pid_t child = -1;

        // A dialog is under way, from the request until its callback has
        // been called. Under the mutex.
        bool busy = false;

        // The dialog under way was closed (close_file_dialog(), the window
        // going): no program is started for it any more, as when it asks a
        // second time. Under the mutex.
        bool closed = false;

        // The last dialog's waiting thread. Only show_file_dialog() and the
        // destructor touch it, which the window's owner keeps apart.
        std::thread thread;

        file_chooser() = default;
        file_chooser(const file_chooser &) = delete;
        file_chooser &operator=(const file_chooser &) = delete;

        ~file_chooser()
        {
            {
                std::lock_guard lock(mutex);
                closed = true;
                if (child > 0)
                    kill(child, SIGTERM);
            }
            if (thread.joinable())
                thread.join();
        }
    };

} // namespace platform::wayland

namespace platform
{
    namespace
    {
        std::unexpected<file_dialog_error> failure(std::string message, bool unavailable = false)
        {
            return std::unexpected(file_dialog_error{.unavailable = unavailable, .message = std::move(message)});
        }

        // The program's full path, from $PATH; empty when it is not there.
        std::string find_program(std::string_view name)
        {
            const char *path = std::getenv("PATH");
            std::string_view directories = path != nullptr && path[0] != '\0' ? path : "/usr/local/bin:/usr/bin:/bin";
            while (!directories.empty())
            {
                const std::size_t end = directories.find(':');
                const std::string_view directory = directories.substr(0, end);
                directories = end == std::string_view::npos ? std::string_view() : directories.substr(end + 1);
                if (directory.empty())
                    continue;
                const std::string candidate = std::string(directory) + "/" + std::string(name);
                if (access(candidate.c_str(), X_OK) == 0)
                    return candidate;
            }
            return {};
        }

        // kdialog on KDE, where it fits in; zenity anywhere else, and each
        // where the other is missing.
        std::optional<std::pair<chooser_program, std::string>> choose_program()
        {
            const char *desktop = std::getenv("XDG_CURRENT_DESKTOP");
            const bool kde = desktop != nullptr && std::strstr(desktop, "KDE") != nullptr;

            const std::string zenity = find_program("zenity");
            const std::string kdialog = find_program("kdialog");
            if (!kdialog.empty() && (kde || zenity.empty()))
                return std::pair{chooser_program::kdialog, kdialog};
            if (!zenity.empty())
                return std::pair{chooser_program::zenity, zenity};
            return std::nullopt;
        }

        // Runs the program to the end and reads what it chose. Its stdout
        // comes back through a pipe; its stderr, where GTK and Qt chatter,
        // goes nowhere.
        file_dialog_result run(wayland::file_chooser &chooser, const std::string &program,
                               const std::vector<std::string> &command, const file_dialog &dialog,
                               std::string &text)
        {
            int output[2] = {-1, -1};
            if (pipe(output) != 0)
                return failure(std::format("pipe failed: {}", std::strerror(errno)));
            fcntl(output[0], F_SETFD, FD_CLOEXEC);

            posix_spawn_file_actions_t actions;
            posix_spawn_file_actions_init(&actions);
            posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
            posix_spawn_file_actions_addclose(&actions, output[1]);
            posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

            std::vector<char *> arguments;
            for (const std::string &argument : command)
                arguments.push_back(const_cast<char *>(argument.c_str()));
            arguments.push_back(nullptr);

            pid_t child = -1;
            int spawned = 0;
            bool closed = false;
            {
                // Under the mutex, so that close_file_dialog() finds the
                // child as soon as it exists.
                std::lock_guard lock(chooser.mutex);
                closed = chooser.closed;
                if (!closed)
                    spawned = posix_spawn(&child, program.c_str(), &actions, nullptr, arguments.data(), environ);
                if (!closed && spawned == 0)
                    chooser.child = child;
            }
            posix_spawn_file_actions_destroy(&actions);
            close(output[1]);
            if (closed)
            {
                close(output[0]);
                return std::vector<std::filesystem::path>{};
            }
            if (spawned != 0)
            {
                close(output[0]);
                return failure(std::format("cannot start {}: {}", command.front(), std::strerror(spawned)));
            }

            // Until the program closes its end: when it exits.
            text.clear();
            char buffer[4096];
            while (true)
            {
                const ssize_t got = read(output[0], buffer, sizeof(buffer));
                if (got > 0)
                    text.append(buffer, static_cast<std::size_t>(got));
                else if (got == 0 || errno != EINTR)
                    break;
            }
            close(output[0]);

            // Reaped under the mutex, so that close_file_dialog() never
            // signals a process id that has been given to another process.
            int status = 0;
            {
                std::lock_guard lock(chooser.mutex);
                while (waitpid(child, &status, 0) < 0 && errno == EINTR)
                {
                }
                chooser.child = -1;
            }

            // 0 chosen, 1 cancelled (both programs); a signal is
            // close_file_dialog() or the window going.
            if (WIFSIGNALED(status))
                return std::vector<std::filesystem::path>{};
            if (!WIFEXITED(status))
                return failure(command.front() + " ended abnormally");
            if (WEXITSTATUS(status) == 1)
                return std::vector<std::filesystem::path>{};
            if (WEXITSTATUS(status) != 0)
                return failure(std::format("{} failed (exit status {})", command.front(), WEXITSTATUS(status)));
            return parse_chooser_output(text, dialog);
        }
    } // namespace

    void show_file_dialog(native_window &owner, file_dialog dialog, file_dialog_callback done)
    {
        const auto program = choose_program();
        if (!program)
        {
            if (done)
                done(failure("there is no file dialog: neither zenity nor kdialog is installed", true));
            return;
        }

        if (!owner.file_chooser)
            owner.file_chooser = std::make_shared<wayland::file_chooser>();
        wayland::file_chooser &chooser = *owner.file_chooser;
        {
            std::lock_guard lock(chooser.mutex);
            if (chooser.busy)
            {
                if (done)
                    done(failure("a file dialog is open for this window already"));
                return;
            }
            chooser.busy = true;
            chooser.closed = false;
        }

        // The last dialog's thread has finished its work (busy was clear);
        // joined before its object is reused.
        if (chooser.thread.joinable())
            chooser.thread.join();

        chooser.thread = std::thread([&chooser, which = program->first, path = program->second,
                                      dialog = std::move(dialog), done = std::move(done)]() mutable {
            std::string output;
            file_dialog_result result = run(chooser, path, chooser_command(which, dialog), dialog, output);

            // Neither program adds the extension, so they asked about
            // replacing the name as typed, not the one with it added: when
            // that one exists, the dialog comes again with it filled in, and
            // the program asks about it then (or the user picks another).
            while (result && !result->empty())
            {
                const std::filesystem::path unconfirmed = unconfirmed_replacement(output, dialog);
                if (unconfirmed.empty())
                    break;
                dialog.directory = unconfirmed.parent_path();
                dialog.file_name = path_to_utf8(unconfirmed.filename());
                result = run(chooser, path, chooser_command(which, dialog), dialog, output);
            }
            {
                std::lock_guard lock(chooser.mutex);
                chooser.busy = false;
            }
            if (done)
                done(std::move(result));
        });
    }

    void close_file_dialog(native_window &owner)
    {
        if (!owner.file_chooser)
            return;
        std::lock_guard lock(owner.file_chooser->mutex);
        owner.file_chooser->closed = owner.file_chooser->busy;
        if (owner.file_chooser->child > 0)
            kill(owner.file_chooser->child, SIGTERM);
    }

} // namespace platform
