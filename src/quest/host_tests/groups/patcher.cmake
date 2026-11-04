# The libr15 build-time patcher's tests (docs/adr/0021) and the pinned programs that read the real store files.

# ── test-quest-patcher ──────────────────────────────────────────────────────────────────────────────────────────────────
# The stub codec against binutils-checked vectors, the patch table and the build ID derived from it, and the tool on a synthetic
# ELF built in memory (the byte diff, every refusal with its row id, the extra-rows file, the command line). The tool itself is
# built by `just quest-patcher` (NEVR_QUEST_PATCH_LIBR15). _GLIBCXX_ASSERTIONS: an out-of-bounds operator[] or an empty optional
# dereference in the patcher aborts the test that reached it, instead of reading past the buffer and passing.
nevr_ht_group(patcher)
set(_pf HOST PTHREAD DEFS _GLIBCXX_ASSERTIONS)
set(_patcher_scratch "${CMAKE_BINARY_DIR}/test-scratch")
add_test(NAME patcher_scratch_fresh COMMAND ${CMAKE_COMMAND} -DDIR=${_patcher_scratch} -P ${NEVR_HT_DIR}/scripts/fresh_dir.cmake)
add_test(NAME patcher_scratch_removed COMMAND ${CMAKE_COMMAND} -E remove_directory ${_patcher_scratch})
set_tests_properties(patcher_scratch_fresh PROPERTIES FIXTURES_SETUP patcher_scratch LABELS patcher)
set_tests_properties(patcher_scratch_removed PROPERTIES FIXTURES_CLEANUP patcher_scratch LABELS patcher)
nevr_ht_test(stub_codec_test GROUP patcher ${_pf} TIMEOUT 60 SRC src/quest/tests/stub_codec_test.cpp)
nevr_ht_test(libr15_patch_table_test GROUP patcher ${_pf} TIMEOUT 60
  SRC src/quest/tests/libr15_patch_table_test.cpp src/quest/patch/libr15_patch_identity.cpp src/quest/data/sha256.cpp)
nevr_ht_test(libr15_patcher_test GROUP patcher ${_pf} TIMEOUT 300 FIXTURE_REQUIRED patcher_scratch
  ARGS "${_patcher_scratch}" src/quest/tests/fixtures "${NEVR_QUEST_PATCH_LIBR15}"
  SRC src/quest/tests/libr15_patcher_test.cpp src/quest/patch/libr15_patcher.cpp src/quest/patch/libr15_extra_rows.cpp
      src/quest/patch/libr15_patcher_cli.cpp src/quest/patch/libr15_patch_identity.cpp src/quest/data/sha256.cpp
      src/quest/sentinel/arm64_reloc.cpp)

# ── test-quest-patcher-pinned ───────────────────────────────────────────────────────────────────────────────────────────
# The shipped patcher on the real store libr15.so (docs/adr/0021 test 2, pinned). The recipe runs the program by name, with the
# store file, the tool and the scratch directory it names, so it is built here and is not a ctest test.
nevr_ht_group(patcher-pinned)
nevr_ht_test(libr15_patcher_pinned_test GROUP patcher-pinned PTHREAD NO_TEST COPTS -O1
  SRC src/quest/tests/libr15_patcher_pinned_test.cpp src/quest/patch/libr15_patch_identity.cpp src/quest/data/sha256.cpp)
