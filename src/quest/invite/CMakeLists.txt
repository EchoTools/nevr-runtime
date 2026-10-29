# The Quest invite service (#966 slice S3, docs/adr/0017): the Quest transport and the start and stop the sentinel calls. The
# service itself is the shared runtime code (src/runtime/patch/invite_service.cpp), compiled here for the Quest with its
# transport injected through Settings::transport_factory. invite_transport_curl.cpp is linked because the service's default
# (the PC transport) names it; the Quest run never selects it.
add_library(nevr_quest_invite STATIC
  "${CMAKE_CURRENT_SOURCE_DIR}/invite_transport_quest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/invite_run.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/patch/invite_service.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/patch/invite_transport_curl.cpp")
target_include_directories(nevr_quest_invite PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_link_libraries(nevr_quest_invite PUBLIC nevr_quest_token_auth nlohmann_json::nlohmann_json CURL::libcurl)
# Hidden visibility: linked into the sentinel, nothing here may become an export (tests/quest TestExportAllowlist).
target_compile_options(nevr_quest_invite PRIVATE -Wall -Wextra -Werror -fvisibility=hidden -fvisibility-inlines-hidden)
