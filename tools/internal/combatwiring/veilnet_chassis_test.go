package combatwiring

import (
	"strings"
	"testing"
)

// #2242 (EchoCombat 2.0.5 and 2.0.8): SERIES-10 ONLY's recursion guard and the server's standard-chassis flag. The decision is
// nevr_combat::gate::SwapChassisWanted and FlagForcedZero (veilnet_gate.h, test_veilnet_gate.cpp). These pin that veilnet.cpp takes
// them: the depth around each of our calls, the flag hook with its prologue and call-site check, and the replay, counted in the
// depth like our other chassis calls.
func TestVeilnetChassisRecursionGuardAndFlagHook(t *testing.T) {
	src := code(t, veilnet)
	if !strings.Contains(src, "thread_local int t_chassis_depth = 0;") {
		t.Error("t_chassis_depth is not a thread-local counter of our own chassis calls")
	}
	loadApply := body(t, src, "void __fastcall HookedLoadApply(uint64_t cs, uint64_t player, uint64_t item, uint64_t slot, int flag) {")
	if !strings.Contains(loadApply, "++t_chassis_depth;") || !strings.Contains(loadApply, "--t_chassis_depth;") || !strings.Contains(loadApply, "SwapChassis(cs)") {
		t.Errorf("HookedLoadApply does not count its own call or does not ask SwapChassis: %s", loadApply)
	}
	applyItem := body(t, src, "void __fastcall HookedApplyItem(uint64_t cs, uint64_t player, uint64_t item, uint64_t slot, int flag) {")
	if !strings.Contains(applyItem, "++t_chassis_depth;") || !strings.Contains(applyItem, "SwapChassis(cs)") {
		t.Errorf("HookedApplyItem does not count its own call or does not ask SwapChassis: %s", applyItem)
	}
	swap := body(t, src, "bool SwapChassis(uint64_t cs) {")
	if !strings.Contains(swap, "SwapChassisWanted(") || !strings.Contains(swap, "t_chassis_depth") {
		t.Errorf("SwapChassis is not the header's decision behind the depth guard: %s", swap)
	}
	netFlag := body(t, src, "int __fastcall HookedNetFlag(uint64_t netgame, uint64_t sym) {")
	if !strings.Contains(netFlag, "FlagForcedZero(sym, ret,") || !strings.Contains(netFlag, "_ReturnAddress()") {
		t.Errorf("HookedNetFlag does not answer 0 by its caller's return address: %s", netFlag)
	}
	hook := body(t, src, "bool HookApplyItem() {")
	for _, want := range []string{"kNetFlagHead", "kLoadApplyFlagRetRva", "ret[-CALL_REL32_LEN] == CALL_REL32_OPCODE", "DetourAttach(&(PVOID &)g_real_net_flag, (PVOID)HookedNetFlag);", "reason=call_site_mismatch", "reason=bytes_mismatch"} {
		if !strings.Contains(hook, want) {
			t.Errorf("HookApplyItem does not check or attach the flag hook (%s)", want)
		}
	}
	rule := body(t, src, "void ApplyChassisRule() {")
	if !strings.Contains(rule, "StandardChassisSet(g_chassis_calls[0].cs)") || !strings.Contains(rule, "++t_chassis_depth;") {
		t.Errorf("ApplyChassisRule does not watch the server's standard flag or count its replay: %s", rule)
	}
}
