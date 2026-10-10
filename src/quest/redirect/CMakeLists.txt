# Config-string redirect for the Quest client: the decision logic and the CJson::TString hook
# adapter. A static library the sentinel links when it enables the redirect; the host test is
# built by `just test-quest-redirect` (it needs dlopen fixtures and a host libc, not the NDK).
#
# The hook backend comes from nevr_quest_got_hook, the one library that holds it.
#
# tstring_thunks.cpp includes callback_thunk.h, so it is built without exception support: its
# frames carry no personality and no LSDA, and a game exception passes through them (see the
# contract in sentinel/callback_thunk.h). Everything that allocates or can throw is in the other
# files, which the thunk reaches only through the noexcept ApplyFn pointer declared in
# tstring_thunks.h (set from hook_adapter.cpp).
add_library(nevr_quest_redirect STATIC
  service_redirector.cpp
  hook_adapter.cpp
  tstring_thunks.cpp)
set_source_files_properties(tstring_thunks.cpp PROPERTIES COMPILE_OPTIONS "-fno-exceptions")

target_include_directories(nevr_quest_redirect PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_include_directories(nevr_quest_redirect PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../sentinel")
target_link_libraries(nevr_quest_redirect PUBLIC
  nevr_quest_config
  nevr_quest_got_hook
  nevr_quest_stable_string_pool)
# Hidden visibility: linked into the sentinel, nothing here may become an export
# (tests/quest TestExportAllowlist).
target_compile_options(nevr_quest_redirect PRIVATE -Wall -Wextra -Werror
  -fvisibility=hidden -fvisibility-inlines-hidden)
