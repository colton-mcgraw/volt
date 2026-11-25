# volt_history: change history. Two layers that do not know each other:
# undo and redo over a document in memory (<history/stack.hpp>,
# <history/record.hpp>; header-only), and the snapshots and branches of a
# project's files kept on disk (<history/repository.hpp>,
# <history/workspace.hpp>, with <history/sha256.hpp> naming what is stored).
# Plain data on files: no GPU, no window, no interface, and usable from a
# worker thread.
add_library(volt_history STATIC
  sha256.cpp
  repository.cpp
  workspace.cpp
)

target_link_libraries(volt_history PUBLIC volt_core volt_json)
volt_set_warnings(volt_history)
