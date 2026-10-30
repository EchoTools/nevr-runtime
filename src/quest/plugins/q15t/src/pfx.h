// pfx (issue #1631): the particle-effect renderer's gate, the switch every pfx* module reads.
//
// The source build keeps its renderer off unless a switch file says otherwise (its On(): pfx.on in the data directory, or a
// debug property). The working build has the renderer on, so this gate is ON by default, and `pfx.off` in the q15t data
// directory (module_switch.h) turns it off. The gate installs no hook and writes no byte: it only decides, once per run, and
// logs the decision. The renderer the gate enables is not ported; docs/quest/q15t/modules/pfx.md says what is missing.
#pragma once

namespace q15t {

enum class PfxGateReason { kDefault, kOffFile };

struct PfxGateDecision {
  bool on;
  PfxGateReason reason;
};

// The decision from one fact: whether `pfx.off` is present in the data directory. Pure, so the host test drives both branches.
PfxGateDecision DecidePfxGate(bool off_file_present);

// Reads the switch, stores the decision for PfxGateOn(), and logs one INFO line naming it. Called once from NvrPluginInitEx on the
// pinned build (plugin.cpp). Returns the decision it stored.
PfxGateDecision InitPfxGate();

// False until InitPfxGate has run; then the stored decision. The pfx* modules read this and do nothing while it is false.
bool PfxGateOn();

// Test seam: forgets the stored decision, so PfxGateOn() reads false again.
void ResetPfxGateForTest();

}  // namespace q15t
