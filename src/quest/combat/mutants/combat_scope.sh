# combat_scope: each rule of src/quest/combat/combat_scope.cpp removed once; src/quest/tests/combat_scope_test.cpp must fail on every one.
# runm <module> <name> <file under src/quest/combat> <sed expression>
runm combat_scope sc_look_every_call   combat_scope.cpp 's/^  if (now < g_nextLookMs.load(std::memory_order_relaxed)) return;$//'
runm combat_scope sc_lobby_not_counted combat_scope.cpp 's/\/\*lobby=\*\/true/\/*lobby=*\/false/'
runm combat_scope sc_arena_not_winning combat_scope.cpp 's/== level::Kind::kCombatOrLobby;/!= level::Kind::kNone;/'
runm combat_scope sc_starts_open       combat_scope.cpp 's/std::atomic<bool> g_in{false};/std::atomic<bool> g_in{true};/'
runm combat_scope sc_no_change_gate    combat_scope.cpp 's/^  if (in == g_in.load(std::memory_order_relaxed)) return;$//'
runm combat_scope sc_change_swapped    combat_scope.cpp 's/{"from", in ? 0 : 1}, {"to", in ? 1 : 0}/{"from", in ? 1 : 0}, {"to", in ? 0 : 1}/'
runm combat_scope sc_enroll_twice      combat_scope.cpp 's/^  if (g_enrolled.exchange(true, std::memory_order_acq_rel)) return true;$//'
runm combat_scope sc_refusal_kept      combat_scope.cpp 's/^  if (!added) g_enrolled.store(false, std::memory_order_release);$//'
runm combat_scope sc_refusal_quiet     combat_scope.cpp 's/added ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "combat_scope_enroll"/sentinel::LogLevel::kInfo, "combat_scope_enroll"/'
runm combat_scope sc_spacing           combat_scope.h 's/kLookEveryMs = 250/kLookEveryMs = 2500/'
