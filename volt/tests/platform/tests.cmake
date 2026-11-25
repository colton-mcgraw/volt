# Tests of the platform layer that need no window: what the file dialogs are
# built from (filters, extensions, UTF-8 paths, and the zenity and kdialog
# command lines of the Linux backend, checked on every system). Showing a
# dialog needs a user, so the dialogs themselves are not run here.
# (platform::is_web_url() is tested with the windows, app/windows_tests.cpp.)
volt_add_test(volt_platform_file_dialog_tests platform/file_dialog_tests.cpp)
target_link_libraries(volt_platform_file_dialog_tests PRIVATE volt_platform)
