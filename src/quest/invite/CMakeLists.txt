# The Quest invite service (docs/adr/0017, #966): the PC's invite_service.cpp and its libcurl transport, compiled for the Quest, with
# the Quest transport (src/quest/invite) injected through Settings::transport_factory. The sentinel links it (src/quest/integration);
# the link test below proves it links and that its off path logs why. Included from src/quest/CMakeLists.txt, after CURL and
# nlohmann-json are found.
add_library(nevr_quest_invite STATIC
  "${CMAKE_CURRENT_SOURCE_DIR}/invite_transport_quest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/invite_run.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/patch/invite_service.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/patch/invite_transport_curl.cpp")
target_include_directories(nevr_quest_invite PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_link_libraries(nevr_quest_invite PUBLIC nevr_quest_token_auth nlohmann_json::nlohmann_json CURL::libcurl)
# Hidden visibility: linked into the sentinel, nothing here may become an export (tests/quest TestExportAllowlist).
target_compile_options(nevr_quest_invite PRIVATE -Wall -Wextra -Werror -fvisibility=hidden -fvisibility-inlines-hidden)

add_executable(invite_service_link_test "${CMAKE_CURRENT_SOURCE_DIR}/../tests/invite_service_link_test.cpp")
target_link_libraries(invite_service_link_test PRIVATE nevr_quest_invite)
target_compile_options(invite_service_link_test PRIVATE -Wall -Wextra -Werror)
