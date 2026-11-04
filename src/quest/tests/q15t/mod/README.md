Each module's host test is `src/quest/tests/q15t/mod/<name>.cmake`, included by `src/quest/tests/q15t/CMakeLists.txt` (a new file is
picked up at the next configure). It runs in both configures of that project, so one file holds the module's host test programs
and, for a module the repack checks, its arm64 checks. Example:

    q15t_host_test(<name>_test TEST <name>_test.cpp SRC <name> import_hook got_swap safe_read plugin_log LIBS dl)
    q15t_arm64_object(<name>)

`q15t_host_test` builds the test file with `src/<module>.cpp` for each name after `SRC` (flags, include directories, definitions,
link options, a timeout and program arguments are keywords; they are listed above the function in `CMakeLists.txt`) and adds the
ctest test `<name>.<program>` with the label `<name>`. `q15t_arm64_object` compiles `src/<name>.cpp` with the NDK clang and adds the
test that the object has no static initializer. The scripts under `check/` read built objects and programs.

`just test-quest-q15t <name>` builds and runs only that module (the `mod_<name>` target and the `<name>` label).

Mutants for it go in `src/quest/tests/q15t/mutants.d/<name>.sh` as `runm <name> <mutant> <file> '<sed expression>'` lines.
