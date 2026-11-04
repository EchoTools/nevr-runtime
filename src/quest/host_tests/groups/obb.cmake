# test-quest-obb and test-quest-obb-pinned: the store OBB extractor (src/quest/obb, docs/adr/0042-quest-store-variant-built-from-store-apk-and-obb.md).
# The tests link the nevr_quest_obb library of the host tools (src/quest/host) and build their images in the test, with nettle's
# own encrypt side. _GLIBCXX_ASSERTIONS comes with the library.

if(NEVR_QUEST_OBB_EXTRACT)
  # ── obb ───────────────────────────────────────────────────────────────────────────────────────────────────────────────
  nevr_ht_group(obb)
  set(_obb_env NEVR_TEST_TMP=${CMAKE_BINARY_DIR}/obb)
  foreach(t obb_footer_test obb_key_test obb_cipher_test fat16_test stock_pins_test stock_data_flow_test)
    nevr_ht_test(${t} GROUP obb PTHREAD TIMEOUT 300 ENV ${_obb_env} LIBS nevr_quest_obb SRC src/quest/tests/${t}.cpp)
  endforeach()

  # ── obb-pinned ────────────────────────────────────────────────────────────────────────────────────────────────────────
  # The extractor on the real store OBB. The recipe runs it by name with the OBB, the pin file and a scratch directory, so it is
  # built here and is not a ctest test.
  nevr_ht_group(obb-pinned)
  nevr_ht_test(obb_pinned_test GROUP obb-pinned PTHREAD NO_TEST LIBS nevr_quest_obb SRC src/quest/tests/obb_pinned_test.cpp)
endif()
