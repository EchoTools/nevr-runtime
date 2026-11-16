// Package measureopt holds the source-shape checks of the script-hook measurement option (ADR 0037, ADR 0040 decision 1;
// src/runtime/hook/script_hook_calls.h). The option's behaviour is driven by src/runtime/tests/test_script_hooks.cpp in the
// mingw-measure build; what a host test cannot see is the configure-time refusal, which only CMake runs, so these tests read the
// tree (rank 3 of docs/standards/verification.md).
package measureopt
