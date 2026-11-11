package combatwiring

import (
	"regexp"
	"strings"
	"testing"
)

// #2358: the EC network client's rules (the packet and its types, the receive test and its cap, the tokens, the dedupe ring, the
// server pick, the client branch with the carried-type mask, the use watcher, the tac-mod parse and the tac-mod hide's decision, settle
// and compare-and-swap write) are one definition, src/runtime/patch/ec_rules.h, which test_ec_rules.cpp drives on PC and ec_test.cpp on
// Quest. veilnet.cpp hooks the game and links no test binary, so these pin its wiring: what the receive hook takes out, the client
// branch, the use watcher, the server pick and the parse go through the header with all four types carried; TacModsGuarded decides
// through it, applies LOADOUTS CLASSIC and StationView, then writes through it; the Quest's ec.cpp calls the same header only where it
// called its own rules, behind the combat scope; and neither port keeps a copy of a rule.

const (
	questEc     = "src/quest/combat/ec.cpp"
	questEcCore = "src/quest/combat/ec_core.h"
	ecRules     = `#include "runtime/patch/ec_rules.h"`
)

func TestBothPortsIncludeTheSharedEcRules(t *testing.T) {
	t.Parallel()
	for _, path := range []string{veilnet, questEcCore} {
		if !strings.Contains(code(t, path), ecRules) {
			t.Errorf("%s no longer includes the shared EC rules", path)
		}
	}
	if !strings.Contains(code(t, questEc), `#include "quest/combat/ec_core.h"`) {
		t.Errorf("%s no longer includes ec_core.h, which carries the shared EC rules", questEc)
	}
}

func TestTheReceiveHookTakesOutOnlyWhatTheHeaderCallsOurs(t *testing.T) {
	t.Parallel()
	fn := body(t, code(t, veilnet), "int WSAAPI Hooked_recvfrom(")
	ordered(t, "Hooked_recvfrom", fn,
		"ApplyClassic();",
		"for (int consumed = 0;; ++consumed) {",
		"int n = Real_recvfrom(s, buf, len, flags, f, fl);",
		"if (!ecr::TakeOut(buf, n, len < 0 ? 0ull : static_cast<unsigned long long>(len), (flags & MSG_PEEK) != 0, fl != nullptr,",
		"consumed, &p))",
		"return n;",
		"if (f->sa_family == AF_INET) Handle(s, p, *(const sockaddr_in *)f);",
		"*fl = saved;",
		"if (ioctlsocket(s, FIONREAD, &waiting) != 0 || waiting == 0) {")
	if strings.Contains(fn, "MAGIC") || strings.Contains(fn, "memcpy(&p") {
		t.Errorf("Hooked_recvfrom tests the packet itself:\n%s", fn)
	}
}

func TestTheSendHookCountsDestinationsOnAClientThroughTheHeader(t *testing.T) {
	t.Parallel()
	src := code(t, veilnet)
	if !strings.Contains(body(t, src, "int WSAAPI Hooked_sendto("), "if (!g_server) SawSend(s, to);") {
		t.Error("Hooked_sendto no longer counts a destination only on a client")
	}
	ordered(t, "SawSend", body(t, src, "void SawSend(SOCKET s, const sockaddr *to) {"),
		"if (!to || to->sa_family != AF_INET) return;",
		"AcquireSRWLockExclusive(&g_lock);",
		"g_ec.SawSend(s, a->sin_addr.s_addr, a->sin_port);",
		"ReleaseSRWLockExclusive(&g_lock);")
	ordered(t, "PickServer", body(t, src, "void PickServer() {"),
		"AcquireSRWLockExclusive(&g_lock);", "g_ec.PickServer();", "ReleaseSRWLockExclusive(&g_lock);")
	ordered(t, "ServerDest", body(t, src, "bool ServerDest(Dest &out) {"),
		"AcquireSRWLockShared(&g_lock);", "bool ok = g_ec.Server(&d);", "ReleaseSRWLockShared(&g_lock);")
	ordered(t, "FirstTime", body(t, src, "bool FirstTime(uint32_t id) {"),
		"AcquireSRWLockExclusive(&g_lock);", "const bool first = g_ec.FirstTime(id);", "ReleaseSRWLockExclusive(&g_lock);")
	if strings.Count(src, "ecr::BasicState<SOCKET> g_ec(ecr::kAllTypes);") != 1 {
		t.Error("PC's EC state no longer carries all four types (ecr::kAllTypes)")
	}
	if !strings.Contains(src, "g_page = g_base + ecr::kPageRva;") {
		t.Error("PC's EC page is no longer the image base + ecr::kPageRva")
	}
}

// The client branch: PC's own match control ('L', 'R', 'G', from the current server only) first, then everything else through the header.
func TestTheClientBranchGoesThroughTheHeader(t *testing.T) {
	t.Parallel()
	fn := body(t, code(t, veilnet), "void Handle(SOCKET s, const Packet &p, const sockaddr_in &from) {")
	ordered(t, "Handle", fn,
		"if (g_server) {",
		"} else if (p.type == 'L') {",
		"} else if (p.type == 'R') {",
		"} else if (p.type == 'G') {",
		"} else {",
		"AcquireSRWLockExclusive(&g_lock);",
		"const ecr::ClientResult r = g_ec.Client(g_page, p, InterruptTime());",
		"ReleaseSRWLockExclusive(&g_lock);",
		"if (r.act == ecr::ClientAct::kMedigun) {",
		"if (MedigunAllowedNow()) MedigunHere(p.id);",
		"} else if (r.act == ecr::ClientAct::kKill) {",
		"if (killed) killed(r.drone);",
		"} else if (r.act == ecr::ClientAct::kToken) {")
	at := strings.Index(fn, "} else {")
	if at < 0 {
		return
	}
	if client := fn[at:]; strings.Contains(client, "FirstTime(") || strings.Contains(client, "AddToken(") {
		t.Errorf("the client branch dedupes or tokens by itself:\n%s", client)
	}
}

func TestTheUseWatcherGoesThroughTheHeader(t *testing.T) {
	t.Parallel()
	fn := body(t, code(t, veilnet), "DWORD WINAPI Tick(LPVOID) {")
	ordered(t, "Tick", fn,
		"uint32_t seen_use[4] = {static_cast<uint32_t>(*uses), static_cast<uint32_t>(*point_uses), static_cast<uint32_t>(*loc_uses),",
		"static_cast<uint32_t>(*oc_uses)};",
		"if (!g_server) {",
		"AcquireSRWLockExclusive(&g_lock);",
		"const int n_used = ecr::WatchUses(g_ec, g_page, seen_use, GetTickCount(), GetCurrentProcessId(), InterruptTime(), used);",
		"ReleaseSRWLockExclusive(&g_lock);",
		"for (int k = 0; k < n_used; ++k) {",
		"SendToServer(type, id);",
		"SendToServer(type, id);",
		"PickServer();",
		"UpdateGearLists();")
	if got := strings.Count(body(t, fn, "for (int k = 0; k < n_used; ++k) {"), "SendToServer(type, id);"); got != 2 {
		t.Errorf("a local use is sent %d times, want 2 (once more, against loss)", got)
	}
}

func TestTheSelectionIsParsedAndStoredThroughTheHeader(t *testing.T) {
	t.Parallel()
	ordered(t, "SawLog", body(t, code(t, veilnet), "void SawLog(const char *fmt, va_list args) {"),
		"if (!ecr::IsProfileFmt(fmt)) return;",
		"const uint64_t sym = ecr::SelectedFromJson(json);",
		"if (!sym) return;",
		"ecr::StoreSelected(g_page, sym);",
		"InterlockedExchange64(&g_selected_pending, (LONG64)sym);")
}

// TacModsGuarded: the decision (with the selection kept whole and the settle) through the header, then LOADOUTS CLASSIC and StationView
// on every look, then the header's compare-and-swap write.
func TestTacModsGuardedDecidesCapsThenWritesThroughTheHeader(t *testing.T) {
	t.Parallel()
	src := code(t, veilnet)
	fn := body(t, src, "int TacModsGuarded(bool server_has_it, uint64_t *now_count) {")
	ordered(t, "TacModsGuarded", fn,
		"EC_TRY {",
		"const uint64_t selected = ecr::Load64(g_page + ecr::kEquipSel);",
		"const bool settled = g_tac_settle.Look(selected, GetTickCount64());",
		"const ecr::TacPlan plan = ecr::PlanTacMods(g_tac_list, reader, list, server_has_it, 0, ecr::OurSelected(selected), settled);",
		"if (!plan.ok) return 0;",
		"uint64_t want = plan.want;",
		"if (LoadoutMode() == LOADOUT_CLASSIC && want > CLASSIC_ITEMS) want = CLASSIC_ITEMS;",
		"StationView(reinterpret_cast<uint8_t *>(list), reinterpret_cast<uint8_t *>(plan.entries), want, 0);",
		"return ecr::WriteTacMods(list, plan, want, true, now_count);",
		"} EC_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {")
	if strings.Contains(fn, "*count = want") || strings.Contains(fn, "count == want") {
		t.Errorf("TacModsGuarded writes or compares the count itself:\n%s", fn)
	}
	logs := body(t, src, "void TacModsFor(bool server_has_it) {")
	if !strings.Contains(logs, `if (r == 3) Log("the server has no EchoCombat, and one of UMBRAL VEIL, OVERCHARGE and POINT is selected: the list is kept whole`) {
		t.Errorf("TacModsFor no longer logs a list kept whole for the selection as kept:\n%s", logs)
	}
	if !strings.Contains(body(t, src, "void UpdateGearLists() {"), "AcquireSRWLockExclusive(&g_lists_lock);") {
		t.Error("UpdateGearLists no longer holds g_lists_lock, which keeps g_tac_list and g_tac_settle")
	}
}

// The Quest calls the header only where it called its own rules: behind the combat scope in both network handlers (arena pays nothing),
// and on the EC thread's step.
func TestTheQuestCallsTheSharedRulesOnlyInCombatScope(t *testing.T) {
	t.Parallel()
	src := code(t, questEc)
	for _, h := range []struct{ sig, first, inScope string }{
		{"ssize_t SendHandler(", "if (!InCombatScope()) return real(fd, buf, len, flags, to, tolen);", "return SendInScope(real, fd, buf, len, flags, to, tolen);"},
		{"ssize_t RecvHandler(", "if (!InCombatScope()) return real(fd, buf, len, flags, from, fromlen);", "return RecvInScope(real, fd, buf, len, flags, from, fromlen);"},
	} {
		fn := strings.TrimSpace(strings.Trim(body(t, src, h.sig), "{}"))
		if !strings.HasPrefix(fn, h.first) {
			t.Errorf("%s no longer starts with the combat scope test:\n%s", h.sig, fn)
		}
		ordered(t, h.sig, fn, h.first, h.inScope)
		if strings.Count(fn, ";") != 2 {
			t.Errorf("%s does more than the scope test and the in-scope call:\n%s", h.sig, fn)
		}
	}
	if !strings.Contains(body(t, src, "ssize_t RecvInScope("), "if (!TakeOut(buf, static_cast<long>(n), len, (flags & MSG_PEEK) != 0, fl != nullptr, consumed, &p)) {") {
		t.Error("RecvInScope no longer takes out only what the shared rules call ours")
	}
	if !strings.Contains(body(t, src, "ssize_t SendInScope("), "g_st.SawSend(fd, a->sin_addr.s_addr, a->sin_port);") {
		t.Error("SendInScope no longer counts destinations through the shared rules")
	}
	step := body(t, src, "void Step(std::uint64_t now100) noexcept {")
	ordered(t, "Step", step,
		"n = WatchUses(g_st, page, L.seenUse, static_cast<std::uint32_t>(nowMs), L.pid, now100, uses);",
		"g_st.PickServer();",
		"const bool settled = L.settle.Look(sel, nowMs);",
		"r = TacModsFor(L.tacList, list, ans, g_hideWrite, &count, g_gear.gen.load(std::memory_order_relaxed), keep, settled);")
	if !strings.Contains(code(t, questEcCore), "return ::nevr_ec::TacModsFor(st, reader, list, serverHasIt, write, nowCount, gen, keepWhole, settled);") {
		t.Error("the Quest's TacModsFor no longer is the shared hide with its SafeRead reader")
	}
}

func TestNeitherEcPortKeepsACopyOfARule(t *testing.T) {
	t.Parallel()
	copies := []*regexp.Regexp{
		regexp.MustCompile(`\bstruct\s+Packet\s*\{`),
		regexp.MustCompile(`0x4c564345|0x4c494556`),
		regexp.MustCompile(`\bbool\s+(Known|Carried|Ours|TakeOut|OursAtEnd|OurSelected|HideSettled|IsProfileFmt)\s*\(`),
		regexp.MustCompile(`\bkMaxConsumePerCall\s*=`),
		regexp.MustCompile(`15000000`),
		regexp.MustCompile(`\bseen\s*\|=\s*s\s*==\s*id\b|\bwas\s*\|=\s*s\s*==\s*id\b`),
		regexp.MustCompile(`%\s*32\]`),
		regexp.MustCompile(`2654435761`),
		regexp.MustCompile(`count\s*>\s*\w*dests\[\w+\]\.count`),
		regexp.MustCompile(`\bAbilitySymbol\s*\(\s*const\s+char`),
		regexp.MustCompile(`\\"ability\\":\\"`),
		regexp.MustCompile(`"smoke",\s*0x`),
		regexp.MustCompile(`sym\s*==\s*0xe32dc7c9d4804fb5`),
		regexp.MustCompile(`\bkNewTacMods\b`),
		regexp.MustCompile(`full\s*-\s*3\b`),
		regexp.MustCompile(`capacity\s*>\s*4096`),
		regexp.MustCompile(`\bkHideSettleMs\s*=`),
		regexp.MustCompile(`__atomic_compare_exchange_n`),
		regexp.MustCompile(`\b(selWas|selChangedMs|g_list_full|g_list_entries|g_seen|g_dests|g_best)\b`),
	}
	for _, path := range []string{veilnet, questEc, questEcCore} {
		src := code(t, path)
		for _, re := range copies {
			if loc := re.FindString(src); loc != "" {
				t.Errorf("%s holds %q, which belongs to src/runtime/patch/ec_rules.h", path, loc)
			}
		}
	}
}

// #2358: a datagram of ours past the per-call cap goes to the game and is counted, not silent. Hooked_recvfrom asks the header whether a
// refused datagram is capped and counts it; NoteCapped writes the first one as a Warning and the count at most every 10 s; Tick flushes the
// count once none has come for 10 s (nevr::BurstThenCount, core/burst_then_count.h).
func TestAPastCapDatagramIsCountedAndTheFirstIsWarned(t *testing.T) {
	t.Parallel()
	src := code(t, veilnet)
	ordered(t, "Hooked_recvfrom", body(t, src, "int WSAAPI Hooked_recvfrom("),
		"if (!ecr::TakeOut(", "if (ecr::Capped(", "NoteCapped(GetTickCount64());", "return n;")
	ordered(t, "NoteCapped", body(t, src, "void NoteCapped(ULONGLONG now) {"),
		"AcquireSRWLockExclusive(&g_capped_lock);", "g_capped_lines.Event(now);", "ReleaseSRWLockExclusive(&g_capped_lock);", "ReportCapped(")
	ordered(t, "ReportCapped", body(t, src, "void ReportCapped(const nevr::BurstThenCount::Line &l) {"),
		"V::kCapped", "LogWarning(", "V::kCount", "datagrams of ours past the cap went to the game")
	ordered(t, "FlushCapped", body(t, src, "void FlushCapped() {"),
		"AcquireSRWLockExclusive(&g_capped_lock);", "g_capped_lines.Flush(now);", "ReleaseSRWLockExclusive(&g_capped_lock);", "ReportCapped(")
	if !strings.Contains(src, "nevr::BurstThenCount g_capped_lines{0, 10000};") {
		t.Error("the past-cap count is not a nevr::BurstThenCount with no burst and a 10 s interval")
	}
	ordered(t, "Tick", body(t, src, "DWORD WINAPI Tick(LPVOID) {"), "FlushCapped();")
}

// ServerDest hands the game the server it picked as the sockaddr it sends to: its socket, the family, the address and the port.
func TestServerDestFillsTheSocketAndTheAddressTheGameSendsTo(t *testing.T) {
	t.Parallel()
	ordered(t, "ServerDest", body(t, code(t, veilnet), "bool ServerDest(Dest &out) {"),
		"out.sock = d.sock;",
		"out.addr.sin_family = AF_INET;",
		"out.addr.sin_addr.s_addr = d.addr;",
		"out.addr.sin_port = d.port;")
}

// #2358: the past-cap count still pending at the stop is written. Stop detaches the hooks first (Uninstall), then writes the count the
// tick has not flushed (FinalCapped: nevr::BurstThenCount::Final), as dronefix.cpp's Stop does (#2349). The Quest's EC module has no
// stop of its own (its install is its only entry point, and nothing uninstalls it), so the Quest has no such call.
func TestStopWritesThePastCapCountThatTheTickHasNotFlushed(t *testing.T) {
	t.Parallel()
	src := code(t, veilnet)
	ordered(t, "Stop", body(t, src, "void Stop() {"), "Uninstall();", "FinalCapped();")
	ordered(t, "FinalCapped", body(t, src, "void FinalCapped() {"),
		"AcquireSRWLockExclusive(&g_capped_lock);", "g_capped_lines.Final(now);", "ReleaseSRWLockExclusive(&g_capped_lock);", "ReportCapped(")
}
