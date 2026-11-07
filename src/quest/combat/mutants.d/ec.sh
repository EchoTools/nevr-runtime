# ec: each rule of src/quest/combat/ec.cpp, ec_core.h, ec_targets.h and sentinel/ec_page.cpp removed or bent once; src/quest/tests/ec_test.cpp
# (and, for the page, ec_page_test) must fail on every one. Sourced by ../mutants.sh, which defines `runm <name> <file> <sed> [<dir> <target>]`.
C=src/quest/combat/ec_core.h
E=src/quest/combat/ec.cpp
T=src/quest/combat/ec_targets.h
P=src/quest/sentinel/ec_page.cpp
# the pure rules (ec_core.h)
runm ec_ours_len        $C "s#if (n != static_cast<long>(sizeof(Packet))) return false;#if (n < 0) return false;#"
runm ec_no_magic2       $C "s# \&\& out->magic2 == kMagic2##"
runm ec_token_restart   $C "s#if (now100 >= __atomic_load_n(expiry, __ATOMIC_SEQ_CST)) __atomic_store_n(count, 0u, __ATOMIC_SEQ_CST);#;#"
runm ec_answer_timeout  $C "s#nowMs - lastAnswerMs < kServerTimeoutMs#nowMs - lastAnswerMs <= kServerTimeoutMs#"
runm ec_dedupe_carried  $C "s#else if (Carried(p.type) \&\& FirstTime(p.id)) {#else if (Carried(p.type)) {#"
runm ec_k_index         $C "s#return (p.id \& 0xffff) + 1;#return p.id + 1;#"
runm ec_types_empty     $C "s#return m ? m : step1;#return m;#"
runm ec_ability_len     $C "s#if (std::strlen(e.name) == n \&\& std::memcmp#if (std::memcmp#"
runm ec_fmt_exact       $C "s# \&\& std::strcmp(fmt, kProfileFmt) == 0##"
runm ec_oursatend       $C "s#if (std::strncmp(name, kNewTacMods\[k\], 64) != 0) return false;##"
runm ec_hide_capacity   $C "s# || capacity > 4096##"
runm ec_hide_fullcap    $C "s#full > capacity || ##"
runm ec_settle          $C "s#nowMs - selChangedMs >= kHideSettleMs#nowMs - selChangedMs > kHideSettleMs#"
runm ec_hide_write      $C "s#  if (write) {#  if (write || true) {#"
runm ec_gate_gen        $C "s#gen.fetch_add(1, std::memory_order_relaxed);##"
runm ec_pick_busiest    $C "s#dests\[i\].count > dests\[b\].count#dests[i].count < dests[b].count#"
runm ec_known_any       $C "s#^inline bool Known(std::uint8_t t) { return .*#inline bool Known(std::uint8_t t) { (void)t; return true; }#"
runm ec_answer_sender   $C "s# \&\& dests\[best\].addr == answerAddr##"
runm ec_dedupe_k        $C "s#else if (p.type == 'K' \&\& FirstTime(p.id)) {#else if (p.type == 'K') {#"
runm ec_carries_gate    $C "s#      if (Carries(p.type)) {#      if (true) {#"
runm ec_equip_word      $C "s#return !WholeWord(words, \"noequip\");#return !WholeWord(words, \"noequip\") || true;#"
runm ec_keepwhole       $C "s#serverHasIt || keepWhole ? full#serverHasIt ? full#"
runm ec_lowerok         $C "s#if (want < count_now \&\& !lowerOk) return 0;#if (want < count_now \&\& !lowerOk \&\& false) return 0;#"
runm ec_hide_gen        $C "s#  if (gen != st.gen) {#  if (false) {#"
runm ec_hide_newlist    $C "s#  if (entries != st.entries) {#  if (false) {#"
# the handlers (ec.cpp): the scope first, then the plugin's rules
runm ec_recv_scope      $E "s#  if (!InCombatScope()) return real(fd, buf, len, flags, from, fromlen);#  if (false) return real(fd, buf, len, flags, from, fromlen);#"
runm ec_send_scope      $E "s#  if (!InCombatScope()) return real(fd, buf, len, flags, to, tolen);#  if (false) return real(fd, buf, len, flags, to, tolen);#"
runm ec_send_v4         $E "s# \&\& to->sa_family == AF_INET##"
runm ec_recv_len        $E "s#len < sizeof(Packet) || ##"
runm ec_recv_peek       $E "s#(flags \& MSG_PEEK) || ##"
runm ec_recv_fromlen    $E "s#(flags \& MSG_PEEK) || fl == nullptr || #(flags \& MSG_PEEK) || #"
runm ec_recv_restore    $E "s#^    \*fl = saved;#    (void)saved;#"
runm ec_recv_flood      $E "s#kMaxConsumePerCall = 256;#kMaxConsumePerCall = 1000;#"
runm ec_tick_fd         $E "s#if (fd == g_gameFd.load(std::memory_order_relaxed)) {#if (true) {#"
runm ec_tick_tid        $E "s#if (tid == g_gameTid.load(std::memory_order_relaxed)) {#if (true) {#"
runm ec_tick_src        $E "s#if (src == g_gameSrc.load(std::memory_order_relaxed)) g_gameTid#if (src || true) g_gameTid#"
runm ec_kill_handler    $E "s#        if (handler != nullptr) handler(killed - 1);#        (void)handler;#"
runm ec_sel_noequip     $E "s#if (sym != 0 \&\& g_equipStore) StoreSelected#if (sym != 0) StoreSelected#"
runm ec_sel_fmt         $E "s#  if (IsProfileFmt(fmt)) {#  if (fmt != nullptr) {#"
runm ec_logv_args       $E "s#  return real(level, flags, fmt, ap);#  return real(level, flags + 1, fmt, ap);#"
runm ec_gear_enter      $E "0,/^  g_gear.Enter();/s#^  g_gear.Enter();##"
# the step
runm ec_hello_period    $E "s#L.nextHello = nowMs + 2000;#L.nextHello = nowMs + 1000;#"
runm ec_send_twice      $E "s#rep < 2#rep < 1#"
runm ec_page_ans        $E "s#  if (ans != L.pageAns) {  // not under the line cap#  if (false) {#"
runm ec_keep_ans        $E "s#g_hideWrite \&\& !ans \&\& OurSelected(sel)#g_hideWrite \&\& OurSelected(sel)#"
runm ec_keep_sel        $E "s#g_hideWrite \&\& !ans \&\& OurSelected(sel)#g_hideWrite \&\& !ans#"
runm ec_settle_use      $E "s#HideSettled(nowMs, L.selChangedMs)#true#"
runm ec_loc_arm         $E "s#^    if (!first) {#    if (!first || true) {#"
runm ec_pow2            $E "s#(Pow2(steps) || steps < L.seenSteps)#(Pow2(steps) || true)#"
# the scope
runm ec_scope_cadence   $E "s#  if (g_nextScopeMs != 0 \&\& nowMs < g_nextScopeMs) return InCombatScope();##"
runm ec_scope_lobby     $E "s#kind = level::Classify(game, /\*lobby=\*/true, \&levelId);#kind = level::Classify(game, /*lobby=*/false, \&levelId);#"
runm ec_scope_kind      $E "s#  const bool in = kind == level::Kind::kCombatOrLobby;#  const bool in = kind != level::Kind::kNone;#"
runm ec_scope_leave     $E "s#      Store32(sentinel::EcPage() + kQuestEcServer, 0);##"
runm ec_scope_every     $E "s#kScopeEveryMs = 250;#kScopeEveryMs = 100;#"
# the install
runm ec_net_both        $E "s#^    g_recvHook.Remove();#    (void)0;#"
runm ec_gear_both       $E "s#^      g_reloadHook.Remove();#      (void)0;#"
runm ec_tick_needed     $E "s#  if (!tick::Add(\&Tick)) return Refused(\"tick_table_full\");#  tick::Add(\&Tick);#"
runm ec_off_switch      $E "s#  if (!switches.on) {#  if (false) {#"
runm ec_build_id        $E "s#if (std::strcmp(id, pinned::LibR15PatchedBuildId()) != 0) return Refused#if (false) return Refused#"
runm ec_default_words   $E "s#switches.words != nullptr ? switches.words : kDefaultWords#switches.words != nullptr ? switches.words : \"\"#"
runm ec_body_check      $E "s#|| have != want) return \"writelog_body_not_known\";#|| (have != want \&\& false)) return \"writelog_body_not_known\";#"
runm ec_stub_check      $E "s#|| have != lib.stub\[i\]) return#|| (have != lib.stub[i] \&\& false)) return#"
runm ec_plat_slot       $E "s#)) return \"slot_not_writelogv\";#)) (void)0;#"
runm ec_plat_r15bind    $E "s#(r15Base == 0 || word != r15Base + g_logLibs\[0\].fn)#(r15Base == 0 || true)#"
runm ec_plat_remap      $E "s#^    g_logHook\[i\].Forget();  // a hook left from a mapping that is gone#    (void)0;#"
runm ec_plat_observer   $E "s#  const bool observing = integration::AddModuleLoadedObserver(\&OnModuleLoaded);#  const bool observing = true;#"
runm ec_plat_at_install $E "s#  for (int i = 1; i < static_cast<int>(kLogLibs); ++i) EnsurePlatform(i, lookup);##"
runm ec_plat_name       $E "s#if (std::strcmp(base, g_logLibs\[i\].module) == 0) EnsurePlatform#if (std::strcmp(base, g_logLibs[i].module) == 0 || true) EnsurePlatform#"
runm ec_plat_installed  $E "s#if (name == nullptr || handle == nullptr || !Installed()) return;#if (name == nullptr || handle == nullptr) return;#"
runm ec_legacy          $E "s#    if (!legacy::Present(file)) continue;#    continue;#"
# the pins: got_pinned_test is no ctest test (it needs the store files), so its two mutants are run by hand against it (the report says how)
# the page: the sentinel writes its own storage, and the export is the alias
runm ec_page_alias      $P "s#visibility(\"default\"), alias#visibility(\"hidden\"), alias#" quest-host-combat ec_page_test
runm ec_page_publish    $P "s#RTLD_NOW | RTLD_NOLOAD | RTLD_GLOBAL#RTLD_NOW | RTLD_NOLOAD#" quest-host-combat ec_page_test
# arena pays nothing, by count (the send / receive handler reads game memory, or scans the levels, before the scope test)
runm ec_arena_reads_before_scope $E 's#^  if (!InCombatScope()) return real(fd, buf, len, flags, to, tolen);#  { bool ok_ = true; (void)SafeRead<std::uint32_t>(reinterpret_cast<std::uintptr_t>(buf), \&ok_); }\n  if (!InCombatScope()) return real(fd, buf, len, flags, to, tolen);#'
runm ec_arena_scans_before_scope $E 's#^  if (!InCombatScope()) return real(fd, buf, len, flags, from, fromlen);#  (void)level::Scan(0, false);\n  if (!InCombatScope()) return real(fd, buf, len, flags, from, fromlen);#'
# off by its key is counted off on the phase line (combat_install_test)
runm ec_off_not_counted $E "s#    return {false, \"off\", true};  // off by its key#    return {false, \"off\", false};  // off by its key#" quest-host-combat combat_install_test
# the arena APK and a plain release run no normal step, so no #2187 module writes a line (combat_install_test, TheRealStepsConsultTheGate)
runm ec_arena_normal_runs src/quest/combat/combat_install.cpp "s#  if (!Active()) {#  if (!Active() \&\& phase == Phase::kEarly) {#" quest-host-combat combat_install_test
