# test-quest-invite and test-quest-invite-transport: the shared invite service on the host (docs/adr/0017, #966). libcurl is the
# host's (pkg-config libcurl); nlohmann-json is the host tree's.

nevr_ht_group(invite)
nevr_ht_test(invite_service_link_test GROUP invite HOST CURL PTHREAD TIMEOUT 120
  SRC src/runtime/patch/invite_service.cpp src/runtime/patch/invite_transport_curl.cpp
      src/quest/tests/invite_service_link_test.cpp)

nevr_ht_group(invite-transport)
nevr_ht_test(invite_transport_quest_test GROUP invite-transport HOST CURL PTHREAD TIMEOUT 300 LIBS ssl crypto
  SRC src/quest/tests/invite_transport_quest_test.cpp
      src/quest/invite/invite_transport_quest.cpp src/quest/invite/invite_run.cpp
      src/quest/auth/curl_http.cpp src/quest/auth/ca_bundle.cpp
      src/runtime/patch/invite_service.cpp src/runtime/patch/invite_transport_curl.cpp)
