# The hardware/environment dump (#335): a diagnostic, off unless nevr-quest.json sets features.hwdump.
#
# nevr_quest_hwdump_hooks holds every frame that is live while libr15 runs under a hook (the handlers, the
# record table they write, the thunk wiring), so it is built without exception support (the contract in
# sentinel/callback_thunk.h). nevr_quest_hwdump is the dump thread and its readers, built with exceptions
# (nlohmann::json) and never on a game call path.
add_library(nevr_quest_hwdump_hooks STATIC
  hwdump_records.cpp
  hwdump_handlers.cpp
  hwdump_install.cpp)
target_include_directories(nevr_quest_hwdump_hooks PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_link_libraries(nevr_quest_hwdump_hooks PRIVATE nevr_quest_got_hook)
target_compile_options(nevr_quest_hwdump_hooks PRIVATE -Wall -Wextra -Werror -fno-exceptions)

add_library(nevr_quest_hwdump STATIC
  hwdump_field.cpp
  hwdump_fs.cpp
  hwdump_os.cpp
  hwdump_report.cpp
  hwdump_android.cpp
  hwdump_run.cpp)
target_include_directories(nevr_quest_hwdump PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../..")
# Vulkan, EGL, GLES, libandroid, libvrapi and OpenXR are reached with dlopen/dlsym only: no DT_NEEDED.
target_link_libraries(nevr_quest_hwdump PUBLIC nevr_quest_hwdump_hooks nevr_quest_atomic_write
  nlohmann_json::nlohmann_json PRIVATE dl log)
target_compile_options(nevr_quest_hwdump PRIVATE -Wall -Wextra -Werror)

# Android-linkable builds of the host tests (cannot execute on the build machine).
add_executable(hwdump_core_test "${CMAKE_CURRENT_SOURCE_DIR}/../tests/hwdump_core_test.cpp")
target_link_libraries(hwdump_core_test PRIVATE nevr_quest_hwdump)
target_compile_options(hwdump_core_test PRIVATE -Wall -Wextra -Werror)
add_executable(hwdump_handlers_test "${CMAKE_CURRENT_SOURCE_DIR}/../tests/hwdump_handlers_test.cpp")
target_link_libraries(hwdump_handlers_test PRIVATE nevr_quest_hwdump_hooks)
target_compile_options(hwdump_handlers_test PRIVATE -Wall -Wextra -Werror -fno-exceptions)
