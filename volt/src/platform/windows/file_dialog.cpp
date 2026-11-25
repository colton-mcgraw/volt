#include <platform/file_dialog.hpp>
#include <platform/windows/window.hpp>

#include <objbase.h>  // CoInitializeEx, CoCreateInstance
#include <shobjidl.h> // IFileDialog and its kin; left out by WIN32_LEAN_AND_MEAN

#include <cstdint>
#include <format>
#include <memory>
#include <system_error>
#include <utility>

namespace platform::windows
{
    namespace
    {
        // What show_file_dialog() posts to the owner, and its window
        // procedure takes back.
        struct request
        {
            file_dialog dialog;
            file_dialog_callback done;
        };

        // A COM interface pointer that releases what it holds, so that no
        // way out of show() leaks one.
        template <typename T>
        class com_ptr
        {
        public:
            com_ptr() = default;
            ~com_ptr() { reset(); }

            com_ptr(const com_ptr &) = delete;
            com_ptr &operator=(const com_ptr &) = delete;

            T *get() const noexcept { return m_pointer; }
            T *operator->() const noexcept { return m_pointer; }

            // For the calls that hand one out (&p in C): whatever it held
            // goes first.
            T **put() noexcept
            {
                reset();
                return &m_pointer;
            }

            void **put_void() noexcept { return reinterpret_cast<void **>(put()); }

            void reset() noexcept
            {
                if (m_pointer != nullptr)
                    std::exchange(m_pointer, nullptr)->Release();
            }

        private:
            T *m_pointer = nullptr;
        };

        // COM on this thread for the length of one dialog. The dialog is a
        // single-threaded apartment object: it wants the apartment of the
        // thread its window belongs to, and that thread to pump messages,
        // which the platform thread does. Initialized here when the thread
        // has not done it (and undone after, so the thread is left as it
        // was found), and counted once more when it has; a thread already
        // in the multithreaded apartment cannot host the dialog.
        class com_apartment
        {
        public:
            com_apartment() : m_result(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}
            ~com_apartment()
            {
                if (SUCCEEDED(m_result))
                    CoUninitialize();
            }

            com_apartment(const com_apartment &) = delete;
            com_apartment &operator=(const com_apartment &) = delete;

            HRESULT result() const noexcept { return m_result; }

        private:
            HRESULT m_result;
        };

        // What the user did not choose: Show() says so with this.
        const HRESULT cancelled_result = HRESULT_FROM_WIN32(ERROR_CANCELLED);

        std::unexpected<file_dialog_error> failure(std::string message, bool unavailable = false)
        {
            return std::unexpected(file_dialog_error{.unavailable = unavailable, .message = std::move(message)});
        }

        std::string com_error(std::string_view what, HRESULT result)
        {
            // FormatMessage knows the HRESULTs of the system's components.
            return std::format("{} failed (0x{:08X}): {}", what, static_cast<std::uint32_t>(result),
                               error_message(static_cast<DWORD>(result)));
        }

        std::expected<std::filesystem::path, std::string> path_of(IShellItem *item)
        {
            wchar_t *name = nullptr;
            const HRESULT result = item->GetDisplayName(SIGDN_FILESYSPATH, &name);
            if (FAILED(result))
                return std::unexpected(com_error("IShellItem::GetDisplayName", result));
            std::filesystem::path path(name);
            CoTaskMemFree(name);
            return path;
        }

        // Builds the dialog, shows it modal to `window` and reads what was
        // chosen. The dialog's loop runs inside Show(), dispatching to every
        // window of the thread (this one is disabled meanwhile).
        file_dialog_result show(native_window &native, HWND window, const file_dialog &dialog)
        {
            // First, so that it goes last: every interface below is released
            // before COM is.
            const com_apartment com;
            if (com.result() == RPC_E_CHANGED_MODE)
                return failure("the window's thread is in COM's multithreaded apartment, where the file dialog "
                               "cannot run");
            if (FAILED(com.result()))
                return failure(com_error("CoInitializeEx", com.result()));

            const bool saving = dialog.mode == file_dialog_mode::save_file;
            const bool folder = dialog.mode == file_dialog_mode::choose_folder;

            com_ptr<IFileDialog> chooser;
            const HRESULT created = CoCreateInstance(saving ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr,
                                                     CLSCTX_INPROC_SERVER, IID_IFileDialog, chooser.put_void());
            if (created == REGDB_E_CLASSNOTREG || created == CLASS_E_CLASSNOTAVAILABLE)
                return failure("this edition of Windows has no file dialog", true);
            if (FAILED(created))
                return failure(com_error("CoCreateInstance(FileDialog)", created));

            // File system paths only (no libraries, no phone over MTP), and
            // the process's working directory left alone.
            FILEOPENDIALOGOPTIONS options = 0;
            if (HRESULT got = chooser->GetOptions(&options); FAILED(got))
                return failure(com_error("IFileDialog::GetOptions", got));
            options |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | FOS_PATHMUSTEXIST;
            switch (dialog.mode)
            {
            case file_dialog_mode::open_file:
                options |= FOS_FILEMUSTEXIST;
                break;
            case file_dialog_mode::open_files:
                options |= FOS_FILEMUSTEXIST | FOS_ALLOWMULTISELECT;
                break;
            case file_dialog_mode::save_file:
                options |= FOS_OVERWRITEPROMPT | FOS_NOREADONLYRETURN;
                break;
            case file_dialog_mode::choose_folder:
                options |= FOS_PICKFOLDERS;
                break;
            }
            if (HRESULT set = chooser->SetOptions(options); FAILED(set))
                return failure(com_error("IFileDialog::SetOptions", set));

            if (!dialog.title.empty())
                chooser->SetTitle(to_wide(dialog.title).c_str());

            // The type list, UTF-16. The strings stay until the function
            // returns, after Show(), whether or not the dialog copies them.
            std::vector<std::wstring> labels;
            std::vector<std::wstring> patterns;
            if (!folder)
            {
                for (const file_filter &filter : dialog.filters)
                {
                    const std::string spec = filter_patterns(filter, ";");
                    if (spec.empty())
                        continue;
                    labels.push_back(to_wide(filter_label(filter, ";")));
                    patterns.push_back(to_wide(spec));
                }
            }
            std::vector<COMDLG_FILTERSPEC> specs;
            for (std::size_t i = 0; i < labels.size(); ++i)
                specs.push_back({labels[i].c_str(), patterns[i].c_str()});
            if (!specs.empty())
            {
                if (HRESULT set = chooser->SetFileTypes(static_cast<UINT>(specs.size()), specs.data()); FAILED(set))
                    return failure(com_error("IFileDialog::SetFileTypes", set));
                chooser->SetFileTypeIndex(1); // 1-based: the first
            }

            // Added to a name typed without one; the dialog switches it to
            // the first extension of whichever type the user picks.
            if (const std::string extension = normalize_extension(dialog.default_extension);
                !extension.empty() && extension != "*")
                chooser->SetDefaultExtension(to_wide(extension).c_str());

            std::error_code error;
            if (!dialog.directory.empty() && std::filesystem::is_directory(dialog.directory, error))
            {
                com_ptr<IShellItem> start;
                if (SUCCEEDED(SHCreateItemFromParsingName(dialog.directory.c_str(), nullptr, IID_IShellItem,
                                                          start.put_void())))
                    chooser->SetFolder(start.get());
            }
            if (!dialog.file_name.empty())
                chooser->SetFileName(to_wide(dialog.file_name).c_str());

            // Reachable for close_file_dialog_message while it is up, which
            // only the dialog's own loop can deliver: on this thread.
            native.file_dialog = chooser.get();
            const HRESULT shown = chooser->Show(window);

            // Nothing in this application destroys a window from inside a
            // callback, but if something did, `native` went with it.
            if (reinterpret_cast<native_window *>(GetWindowLongPtrW(window, GWLP_USERDATA)) == &native)
                native.file_dialog = nullptr;

            if (shown == cancelled_result)
                return std::vector<std::filesystem::path>{};
            if (FAILED(shown))
                return failure(com_error("IFileDialog::Show", shown));

            std::vector<std::filesystem::path> paths;
            if (dialog.mode == file_dialog_mode::open_files)
            {
                com_ptr<IFileOpenDialog> opener;
                com_ptr<IShellItemArray> items;
                if (HRESULT asked = chooser->QueryInterface(IID_IFileOpenDialog, opener.put_void()); FAILED(asked))
                    return failure(com_error("QueryInterface(IFileOpenDialog)", asked));
                if (HRESULT got = opener->GetResults(items.put()); FAILED(got))
                    return failure(com_error("IFileOpenDialog::GetResults", got));
                DWORD count = 0;
                if (HRESULT counted = items->GetCount(&count); FAILED(counted))
                    return failure(com_error("IShellItemArray::GetCount", counted));
                for (DWORD i = 0; i < count; ++i)
                {
                    com_ptr<IShellItem> item;
                    if (HRESULT got = items->GetItemAt(i, item.put()); FAILED(got))
                        return failure(com_error("IShellItemArray::GetItemAt", got));
                    auto path = path_of(item.get());
                    if (!path)
                        return failure(std::move(path).error());
                    paths.push_back(std::move(*path));
                }
            }
            else
            {
                com_ptr<IShellItem> item;
                if (HRESULT got = chooser->GetResult(item.put()); FAILED(got))
                    return failure(com_error("IFileDialog::GetResult", got));
                auto path = path_of(item.get());
                if (!path)
                    return failure(std::move(path).error());
                paths.push_back(std::move(*path));
            }
            return paths;
        }
    } // namespace

    void run_file_dialog(HWND window, native_window *native, LPARAM lparam)
    {
        std::unique_ptr<request> taken(reinterpret_cast<request *>(lparam));
        if (!taken)
            return;

        // A window being created or destroyed answers as cancelled: it is
        // not there to be modal to.
        file_dialog_result result = std::vector<std::filesystem::path>{};
        if (native != nullptr)
        {
            if (native->file_dialog != nullptr)
                result = failure("a file dialog is open for this window already");
            else
                result = show(*native, window, taken->dialog);
        }

        // The request is gone before the callback runs, which may take
        // its time handing the result on.
        file_dialog_callback done = std::move(taken->done);
        taken.reset();
        if (done)
            done(std::move(result));
    }

    void end_file_dialog(native_window &native)
    {
        IFileDialog *dialog = native.file_dialog;
        if (dialog == nullptr)
            return;

        // Show() is to return with this result, as if the user had
        // cancelled. Close() alone was seen to return S_OK and leave the
        // dialog up when it was not the active window (Windows 11, 26200):
        // it ends the dialog's loop only once something else wakes it. So
        // the dialog's own Cancel is posted to its window as well, which
        // does both, and which a dialog already ending ignores.
        dialog->Close(cancelled_result);

        com_ptr<IOleWindow> window;
        HWND handle = nullptr;
        if (SUCCEEDED(dialog->QueryInterface(IID_IOleWindow, window.put_void())) &&
            SUCCEEDED(window->GetWindow(&handle)) && handle != nullptr)
            PostMessageW(handle, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
    }

    void cancel_file_dialogs(HWND window)
    {
        MSG message;
        while (PeekMessageW(&message, window, file_dialog_message, file_dialog_message, PM_REMOVE))
            run_file_dialog(window, nullptr, message.lParam);
    }

} // namespace platform::windows

namespace platform
{

    void show_file_dialog(native_window &owner, file_dialog dialog, file_dialog_callback done)
    {
        // To the owner's thread, which runs it when it next dispatches;
        // from then on the request belongs to the window procedure.
        auto taken = std::make_unique<windows::request>(std::move(dialog), std::move(done));
        if (PostMessageW(owner.handle, windows::file_dialog_message, 0, reinterpret_cast<LPARAM>(taken.get())))
        {
            taken.release();
            return;
        }

        const DWORD code = GetLastError();
        if (taken->done)
            taken->done(windows::failure(std::format("PostMessageW failed ({}): {}", code, windows::error_message(code))));
    }

    void close_file_dialog(native_window &owner)
    {
        // The dialog lives on the owner's thread, so the closing happens
        // there too (windows::end_file_dialog()).
        PostMessageW(owner.handle, windows::close_file_dialog_message, 0, 0);
    }

} // namespace platform
