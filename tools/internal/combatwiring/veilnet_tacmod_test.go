package combatwiring

import (
	"strings"
	"testing"
)

// #2242 (EchoCombat 2.0.8): the tac mod pick also comes from the local player's tac mod slot (the slot setter's filter), because
// nEVR sends the profile itself and the platform module's profile log line is not written under it. This pins that FilterSlot takes
// the local slot's pick through PickTacMod, which asks the gate header and writes the selection the guard reads.
func TestVeilnetTakesTheTacModPickFromTheLocalSlot(t *testing.T) {
	src := code(t, veilnet)
	filter := body(t, src, "uint64_t FilterSlot(int I, void *component, uint32_t idx, uint64_t item) {")
	if !strings.Contains(filter, "if (I == 1 && local) PickTacMod(item);") {
		t.Errorf("FilterSlot does not take the tac mod pick from the local slot: %s", filter)
	}
	pick := body(t, src, "void PickTacMod(uint64_t sym) {")
	for _, want := range []string{"gate::PickFromSlot(sym, ecr::Load64(g_page + ecr::kEquipSel))", "ecr::StoreSelected(g_page, sym);", "g_selected_pending"} {
		if !strings.Contains(pick, want) {
			t.Errorf("PickTacMod does not %s", want)
		}
	}
}
