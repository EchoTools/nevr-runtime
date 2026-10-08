# Quest session router and transports: the shared platform-neutral router
# (src/runtime/compat/session_router.cpp), the loopback WebSocket game server and the verified-TLS
# remote WebSocket client over the libcurl/OpenSSL the Quest vcpkg manifest provides.
#
# Built, linked into test executables, not linked into the sentinel: the hook that points the game at
# the loopback port is gated by ADR 0003. The test sources also run on the host (`just test-quest-router`,
# `just test-quest-tls`); here they only prove the NDK toolchain compiles and links them.

find_package(CURL CONFIG REQUIRED)
set(NEVR_SRC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

add_library(nevr_quest_session_router STATIC
  "${NEVR_SRC_ROOT}/runtime/compat/session_router.cpp")
target_include_directories(nevr_quest_session_router PUBLIC "${NEVR_SRC_ROOT}")
target_link_libraries(nevr_quest_session_router PUBLIC nevr_quest_evr_codec)
target_compile_options(nevr_quest_session_router PRIVATE -Wall -Wextra -Werror)

add_library(nevr_quest_net STATIC
  ws_wire.cpp
  loopback_game_server.cpp
  remote_ws.cpp
  curl_ws_connector.cpp
  session_bridge.cpp
  "${NEVR_SRC_ROOT}/runtime/server/serverdb_uri.cpp")
target_include_directories(nevr_quest_net PUBLIC "${NEVR_SRC_ROOT}")
# nevr_quest_token_auth supplies the CA loader (auth/ca_bundle.cpp) both TLS clients share.
target_link_libraries(nevr_quest_net PUBLIC nevr_quest_session_router nevr_quest_token_auth CURL::libcurl)
target_compile_options(nevr_quest_net PRIVATE -Wall -Wextra -Werror)

foreach(test_name session_router_test ws_wire_test loopback_game_server_test remote_ws_test curl_ws_tls_test)
  add_executable(${test_name} "${CMAKE_CURRENT_SOURCE_DIR}/../tests/${test_name}.cpp")
  target_link_libraries(${test_name} PRIVATE nevr_quest_net)
  target_compile_options(${test_name} PRIVATE -Wall -Wextra -Werror)
endforeach()
