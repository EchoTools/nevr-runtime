# Config-string redirect for the Quest client: the decision logic and the CJson::TString hook
# adapter. A static library the sentinel links when it enables the redirect; the host test is
# built by `just test-quest-redirect` (it needs dlopen fixtures and a host libc, not the NDK).
#
# got_hook.cpp and hook_log.cpp are compiled into the sentinel itself, so this archive leaves
# the sentinel:: symbols undefined for the final link.
add_library(nevr_quest_redirect STATIC
  service_redirector.cpp
  hook_adapter.cpp)

target_include_directories(nevr_quest_redirect PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_include_directories(nevr_quest_redirect PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../sentinel")
target_link_libraries(nevr_quest_redirect PUBLIC
  nevr_quest_config
  nevr_quest_stable_string_pool)
target_compile_options(nevr_quest_redirect PRIVATE -Wall -Wextra -Werror)
