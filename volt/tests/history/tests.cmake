# Tests of the history module. Undo and redo are header-only and need
# nothing but the headers: the stack (transactions, merging, the saved
# marker, limits) and record changes.
volt_add_test(volt_history_stack_tests history/stack_tests.cpp)
volt_add_test(volt_history_record_tests history/record_tests.cpp)

# Snapshots on disk: the hash, the repository (objects, snapshots, branches,
# the log, what differs, verification), and a directory of files snapshotted
# and restored.
foreach(suite IN ITEMS sha256 repository workspace)
  volt_add_test(volt_history_${suite}_tests history/${suite}_tests.cpp)
  target_link_libraries(volt_history_${suite}_tests PRIVATE volt_history)
endforeach()
