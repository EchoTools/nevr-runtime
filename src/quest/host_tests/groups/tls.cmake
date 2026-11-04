# test-quest-tls: the Quest remote WebSocket connector (libcurl over TLS) against real TLS servers on the host. The recipe makes
# the certificates with openssl and starts src/quest/tests/tls_ws_server.py, then runs the program by name with their ports, so the
# program is built here and is not a ctest test.

nevr_ht_group(tls)
nevr_ht_test(curl_ws_tls_test GROUP tls CURL PTHREAD NO_TEST LIBS ssl crypto
  SRC src/quest/net/curl_ws_connector.cpp src/quest/net/remote_ws.cpp src/quest/auth/ca_bundle.cpp
      src/runtime/compat/session_router.cpp src/runtime/compat/evr_codec.cpp src/quest/tests/curl_ws_tls_test.cpp)
