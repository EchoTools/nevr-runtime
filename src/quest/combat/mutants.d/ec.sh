# ec: each rule of src/runtime/patch/ec_rules.h (the EC network rules shared with PC, #2358), src/quest/combat/ec.cpp, ec_core.h, ec_targets.h
# and sentinel/ec_page.cpp removed or bent once; src/quest/tests/ec_test.cpp (and, for the page, ec_page_test) must fail on every one. Sourced
# by ../mutants.sh, which defines `runm <name> <file> <sed> [<dir> <target>]`. PC's GTest (test_ec_rules) drives the same header under Wine;
# its hand mutants are not part of this list.
R=src/runtime/patch/ec_rules.h
C=src/quest/combat/ec_core.h
E=src/quest/combat/ec.cpp
T=src/quest/combat/ec_targets.h
P=src/quest/sentinel/ec_page.cpp
# the shared rules (ec_rules.h)
runm ec_ours_len        $R "s#if (n != static_cast<long>(sizeof(Packet))) return false;#if (n < 0) return false;#"
runm ec_no_magic2       $R "s# \&\& out->magic2 == kMagic2##"
runm ec_known_any       $R "s#    return Carried(t) || t == 'H'#    return true || t == 'H'#"
runm ec_known_c         $R "s# || t == 'C'##"
runm ec_known_l         $R "s# || t == 'L'##"
runm ec_known_r         $R "s# || t == 'R'##"
runm ec_known_g         $R "s# || t == 'G'##"
runm ec_recv_flood      $R "s#kMaxConsumePerCall = 256;#kMaxConsumePerCall = 1000;#"
runm ec_consume_cap     $R "s# || consumed >= kMaxConsumePerCall) return false;# || consumed < 0) return false;#"
runm ec_recv_len        $R "s# || bufLen < sizeof(Packet)# || bufLen < 1#"
runm ec_recv_peek       $R "s#if (peek || #if ((peek \&\& false) || #"
runm ec_recv_fromlen    $R "s#if (peek || !haveFromLen || #if (peek || (!haveFromLen \&\& false) || #"
runm ec_token_restart   $R "s#if (now100 >= __atomic_load_n(expiry, __ATOMIC_SEQ_CST)) __atomic_store_n(count, 0u, __ATOMIC_SEQ_CST);#;#"
runm ec_a_counted       $R "s#            ++answers;#            ;#"
runm ec_m_dedupe        $R "/if (p.type == 'M') {/,/++medi;/ s#if (!FirstTime(p.id)) return#if (false) return#"
runm ec_dedupe_k        $R "/if (p.type == 'K') {/,/++kills;/ s#if (!FirstTime(p.id)) return#if (false) return#"
runm ec_dedupe_carried  $R "s#if (Carried(p.type) \&\& FirstTime(p.id)) {#if (Carried(p.type)) {#"
runm ec_k_index         $R "s#ClientAct::kKill, p.id \& 0xffffu}#ClientAct::kKill, p.id}#"
runm ec_carries_gate    $R "s#            if (!Carries(p.type)) {#            if (false) {#"
runm ec_watch_mask      $R "s#        if (!st.Carries(type)) continue;#        (void)0;#"
runm ec_watch_t         $R "s#        if (type != 'T') {#        if (true) {#"
runm ec_pick_busiest    $R "s#dests\[i\].count > dests\[b\].count#dests[i].count < dests[b].count#"
runm ec_ability_len     $R "s#if (std::strlen(e.name) == n \&\& std::memcmp#if (std::memcmp#"
runm ec_fmt_exact       $R "s# \&\& std::strcmp(fmt, kProfileFmt) == 0##"
runm ec_oursatend       $R "s#if (std::strncmp(name, kNewTacMods\[k\], 64) != 0) return false;##"
runm ec_hide_capacity   $R "s# || capacity > kMaxTacCapacity##"
runm ec_hide_fullcap    $R "s#full > capacity || ##"
runm ec_settle          $R "s#nowMs - selChangedMs >= kHideSettleMs#nowMs - selChangedMs > kHideSettleMs#"
runm ec_settle_hold     $R "s#    if (want < count \&\& !settled) want = count;#    (void)settled;#"
runm ec_hide_write      $R "s#    if (write) {#    if (write || true) {#"
runm ec_hide_cas        $R "s#        if (!__atomic_compare_exchange_n(#        if (__atomic_store_n(reinterpret_cast<std::uint64_t*>(list + 0x10), want, __ATOMIC_SEQ_CST), !__atomic_compare_exchange_n(#"
runm ec_write_same      $R "s#if (!plan.ok || plan.count == want) return 0;#if (!plan.ok) return 0;#"
runm ec_keepwhole       $R "s#serverHasIt || keepWhole ? full#serverHasIt ? full#"
runm ec_keep_result     $R "s#plan.result = serverHasIt ? 1 : keepWhole ? 3 : 2;#plan.result = serverHasIt ? 1 : 2;#"
runm ec_hide_gen        $R "s#    if (gen != st.gen) {#    if (false) {#"
runm ec_hide_newlist    $R "s#    if (entries != st.entries) {  // a new list#    if (false) {  // a new list#"
# the Quest's own (ec_core.h): the words and the gear gate
runm ec_types_empty     $C "s#return m ? m : step1;#return m;#"
runm ec_equip_word      $C "s#return !WholeWord(words, \"noequip\");#return !WholeWord(words, \"noequip\") || true;#"
runm ec_gate_gen        $C "s#gen.fetch_add(1, std::memory_order_relaxed);##"
# the handlers (ec.cpp): the scope first, then the plugin's rules
runm ec_recv_scope      $E "s#  if (!InCombatScope()) return real(fd, buf, len, flags, from, fromlen);#  if (false) return real(fd, buf, len, flags, from, fromlen);#"
runm ec_send_scope      $E "s#  if (!InCombatScope()) return real(fd, buf, len, flags, to, tolen);#  if (false) return real(fd, buf, len, flags, to, tolen);#"
runm ec_send_v4         $E "s# \&\& to->sa_family == AF_INET##"
runm ec_recv_restore    $E "s#^    \*fl = saved;#    (void)saved;#"
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
runm ec_settle_use      $E "s#keep, settled);#keep, settled || true);#"
runm ec_loc_arm         $E "s#^    if (!first) {#    if (!first || true) {#"
runm ec_pow2            $E "s#(Pow2(steps) || steps < L.seenSteps)#(Pow2(steps) || true)#"
# the scope
runm ec_scope_cadence   $E "s#  if (g_nextScopeMs != 0 \&\& nowMs < g_nextScopeMs) return InCombatScope();##"
runm ec_scope_lobby     $E "s#  const bool in = forced >= 0 ? forced != 0 : scope::InCombatOrLobby(look);#  const bool in = forced >= 0 ? forced != 0 : scope::InCombatMap(look);#"
runm ec_scope_kind      $E "s#  const bool in = forced >= 0 ? forced != 0 : scope::InCombatOrLobby(look);#  const bool in = forced >= 0 ? forced != 0 : scope::ClassOf(look) != scope::LookClass::kOther;#"
runm ec_scope_stale_believed $E "s#  const bool in = forced >= 0 ? forced != 0 : scope::InCombatOrLobby(look);#  const bool in = forced >= 0 ? forced != 0 : scope::SeenCombatOrLobby();#"
runm ec_scope_class_unlogged $E "s#  if (in != was || cls != g_lastClass || !g_scopeLogged) {#  if (in != was || !g_scopeLogged) {#"
runm ec_scope_class_dropped $E "s#(in ? scope::LookClass::kCombatOrLobby : scope::LookClass::kOther) : scope::ClassOf(look);#(in ? scope::LookClass::kCombatOrLobby : scope::LookClass::kOther) : scope::LookClass::kOther;#"
runm ec_scope_leave     $E "s#      Store32(sentinel::EcPage() + kQuestEcServer, 0);##"
runm ec_scope_every     $E "s#kScopeEveryMs = 250;#kScopeEveryMs = 100;#"
# the install
runm ec_net_both        $E "s#^    g_recvHook.Remove();#    (void)0;#"
runm ec_gear_both       $E "s#^      g_reloadHook.Remove();#      (void)0;#"
runm ec_scope_live_unchecked $E "/^  if (!scope::Live()) return Refused(\"scope_not_live\");\$/d"
runm ec_scope_live_token $E "s#return Refused(\"scope_not_live\");#return Refused(\"module_not_loaded\");#"
runm ec_scope_live_after_image $E "/^  if (!scope::Live()) return Refused(\"scope_not_live\");\$/d; s#^  if (std::strcmp(id, pinned::LibR15PatchedBuildId()) != 0) return Refused(\"build_id_mismatch\");\$#&\n  if (!scope::Live()) return Refused(\"scope_not_live\");#"
runm ec_scope_live_before_words $E "/^  if (!scope::Live()) return Refused(\"scope_not_live\");\$/d; s#^  if (g_ran.exchange(true, std::memory_order_acq_rel)) return Refused(\"already_ran\");\$#&\n  if (!scope::Live()) return Refused(\"scope_not_live\");#" quest-host-combat combat_install_test
runm ec_scope_live_over_off $E "/^  if (!scope::Live()) return Refused(\"scope_not_live\");\$/d; s#^  if (!switches.on) {\$#  if (!scope::Live()) return Refused(\"scope_not_live\");\n&#"
# The scope row's list of readers (combat_install.cpp, a shared file: run while holding the shared lock): the EC client keeps the row on alone.
runm ecrow_scope_reader src/quest/combat/combat_install.cpp 's/\(|| \|^ *\)g_config\.ec\.on/\1false/' quest-host-combat combat_install_test
runm ec_off_switch      $E "s#  if (!switches.on) {#  if (false) {#"
# the server's statement (#2333)
runm ec_gate_source     $E "s#  ans = nevr_server_features::Client().Current().echoCombat;#  ans = false;#"
runm ec_settings_fwd    $E "s#^  real(netGame, msg);#  real(netGame, nullptr);#"
runm ec_settings_read   $E "s#    statement = nevr_server_features::ReadDocument(text.data(), text.size());#    (void)text;#"
runm ec_settings_copy   $E "s#  if (!g_settingsBad) std::memcpy(g_settingsText, text, static_cast<std::size_t>(size));#  (void)0;#"
runm ec_settings_cap    $E "s#  g_settingsBad = text == nullptr || size > kSettingsCap;#  g_settingsBad = text == nullptr;#"
runm ec_settings_seq    $E "s#^  ++g_settingsSeq;#  (void)0;#"
runm ec_drain_step      $E "s#^  DrainLobbySettings();  // \#2333#  (void)0;#"
runm ec_settings_full   $E "s# \&\& observing \&\& g_settingsOn;# \&\& observing;#"
runm ec_settings_stats  $E "s#    s.answered = nevr_server_features::Client().Current().echoCombat;#    s.answered = false;#"
# ec_settings_slot (the pinned slot's address) is got_pinned_test's, against the store file: the fake image takes its slot from the same pins
runm ec_settings_kind   $T "s#kLobbySettingsSymbol, sentinel::RelocKind::kGlobDat,#kLobbySettingsSymbol, sentinel::RelocKind::kJumpSlot,#"
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
runm ec_arena_scans_before_scope $E 's#^\#include "quest/combat/combat_scope.h"$#&\n\#include "quest/combat/combat_level.h"#; s#^  if (!InCombatScope()) return real(fd, buf, len, flags, from, fromlen);#  (void)level::Scan(0, false);\n  if (!InCombatScope()) return real(fd, buf, len, flags, from, fromlen);#'
# off by its key is counted off on the phase line (combat_install_test)
runm ec_off_not_counted $E "s#    return {false, \"off\", true};  // off by its key#    return {false, \"off\", false};  // off by its key#" quest-host-combat combat_install_test
# the arena APK and a plain release run no normal step, so no #2187 module writes a line (combat_install_test, TheRealStepsConsultTheGate)
runm ec_arena_normal_runs src/quest/combat/combat_install.cpp "s#  if (!Active()) {#  if (!Active() \&\& phase == Phase::kEarly) {#" quest-host-combat combat_install_test
# the past-cap count (#2358): the predicate's boundary (shared rules), and the count the Quest writes (its stat and the summary's capped)
runm ec_capped_pred     $R 's#|| consumed < kMaxConsumePerCall) return false;#|| consumed <= kMaxConsumePerCall) return false;#'
runm ec_capped_count    $E 's#g_capped.fetch_add(1, std::memory_order_relaxed);#(void)0;#'
runm ec_capped_summary  $E 's#{"capped", g_capped.load()}, ##'
# one look per decision (#2382, F1): RefreshScope's `in`, class and level are of the one look it read (ec.cpp ~:534-535, ~:550)
runm ec_one_look_in     $E "s#scope::InCombatOrLobby(look);#scope::InCombatOrLobby(scope::LastLook());#"
runm ec_one_look_class  $E "s#: scope::ClassOf(look);#: scope::ClassOf(scope::LastLook());#"
runm ec_one_look_level  $E "s#sentinel::HexString(levelText, look.level)#sentinel::HexString(levelText, scope::LastLook().level)#"
# the scope line's tokens and level (#2382, F2): the class of a look and its level, as the line names them
runm ec_other_forced    $E "s#: scope::ClassOf(look);#: (in ? scope::LookClass::kOther : scope::ClassOf(look));#"
runm ec_other_token     $E "s#scope::LookClassName(cls)},#cls == scope::LookClass::kOther ? \"combat_or_lobby\" : scope::LookClassName(cls)},#"
runm ec_level_zero      $E "s#sentinel::HexString(levelText, look.level)#sentinel::HexString(levelText, look.level * 0)#"
runm ec_forced_class_swap $E "s#(in ? scope::LookClass::kCombatOrLobby : scope::LookClass::kOther) : scope::ClassOf(look);#(in ? scope::LookClass::kOther : scope::LookClass::kCombatOrLobby) : scope::ClassOf(look);#"
# the stale leave clears the server and the answer word (#2382, F3)
runm ec_stale_leave_keeps $E "s#^    if (!in) {  // leaving#    if (false) {  // leaving#"
