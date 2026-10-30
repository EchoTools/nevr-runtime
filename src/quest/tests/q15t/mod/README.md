Each module's host test is `src/quest/tests/q15t/mod/<name>.sh`, sourced by run_host_tests.sh with `$here`, `$nevr` and `$out` set. Example:

    g++ -std=c++17 -g -O1 -fno-exceptions -fno-rtti -Wall -Wextra -Werror -I"$here/src" -I"$nevr/sdk/include" \
      "$here/tests/<name>_test.cpp" "$here/src/<name>.cpp" ... -o "$out/<name>_test" -ldl
    "$out/<name>_test"

Mutants for it go in `src/quest/tests/q15t/mutants.d/<name>.sh` as `runm <name> <mutant> <file> '<sed expression>'` lines.
`Q15T_MODULE=<name> bash src/quest/tests/q15t/run_host_tests.sh` runs only that module.
