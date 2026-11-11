# module `headset_gates`: each line removes one rule from src/quest/combat/headset_gates.cpp and its test (src/quest/tests/headset_gates_test.cpp) must fail on it.
# This file covers the install's off result only (#2181: every gate switched off by its key is counted as off, not failed, as levelname and
# boosterpool are). The module's other rules have no mutant list yet. Sourced by ../mutants.sh, which defines
# `runm <name> <file> <sed> [<build dir> <test target>]`. Run: src/quest/combat/mutants.sh headset_gates
C=src/quest/combat/headset_gates.cpp
runm hg_off_not_counted $C "s/    return {false, \"off\", true};/    return {false, \"off\"};/"
