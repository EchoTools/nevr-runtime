# test-quest-obb and test-quest-obb-pinned: the store OBB extractor (src/quest/obb, docs/adr/0042-quest-store-variant-built-from-store-apk-and-obb.md).
# The tests link the nevr_quest_obb library of the host tools (src/quest/host) and build their images in the test, with nettle's
# own encrypt side. _GLIBCXX_ASSERTIONS comes with the library.

if(NEVR_QUEST_OBB_EXTRACT)
  # ── obb ───────────────────────────────────────────────────────────────────────────────────────────────────────────────
  nevr_ht_group(obb)
  # NEVR_TEST_TMP is read at configure time (see data.cmake): the recipe sets it to its run root; unset, the build tree's obb/.
  if(DEFINED ENV{NEVR_TEST_TMP})
    set(_obb_env NEVR_TEST_TMP=$ENV{NEVR_TEST_TMP})
  else()
    string(TIMESTAMP _nevr_ht_stamp "%Y%m%dT%H%M%S" UTC)
    string(RANDOM LENGTH 6 _nevr_ht_rand)
    set(_obb_dir "${CMAKE_BINARY_DIR}/obb-${_nevr_ht_stamp}-${_nevr_ht_rand}")
    file(MAKE_DIRECTORY "${_obb_dir}")
    set(_obb_env NEVR_TEST_TMP=${_obb_dir})
  endif()
  nevr_ht_test(obb_footer_test GROUP obb PTHREAD TIMEOUT 300 ENV ${_obb_env} LIBS nevr_quest_obb SRC src/quest/tests/obb_footer_test.cpp)
  nevr_ht_test(obb_key_test GROUP obb PTHREAD TIMEOUT 300 ENV ${_obb_env} LIBS nevr_quest_obb SRC src/quest/tests/obb_key_test.cpp)
  nevr_ht_test(obb_cipher_test GROUP obb PTHREAD TIMEOUT 300 ENV ${_obb_env} LIBS nevr_quest_obb SRC src/quest/tests/obb_cipher_test.cpp)
  nevr_ht_test(fat16_test GROUP obb PTHREAD TIMEOUT 300 ENV ${_obb_env} LIBS nevr_quest_obb SRC src/quest/tests/fat16_test.cpp)
  nevr_ht_test(stock_pins_test GROUP obb PTHREAD TIMEOUT 300 ENV ${_obb_env} LIBS nevr_quest_obb SRC src/quest/tests/stock_pins_test.cpp)
  nevr_ht_test(stock_data_flow_test GROUP obb PTHREAD TIMEOUT 300 ENV ${_obb_env} LIBS nevr_quest_obb SRC src/quest/tests/stock_data_flow_test.cpp)

  # ── obb-pinned ────────────────────────────────────────────────────────────────────────────────────────────────────────
  # The extractor on the real store OBB. The recipe runs it by name with the OBB, the pin file and a scratch directory, so it is
  # built here and is not a ctest test.
  nevr_ht_group(obb-pinned)
  nevr_ht_test(obb_pinned_test GROUP obb-pinned PTHREAD NO_TEST LIBS nevr_quest_obb SRC src/quest/tests/obb_pinned_test.cpp)
endif()
