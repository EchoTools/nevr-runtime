package combatwiring

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// #2337: the drone muzzle rule (the muzzle offset, the back-off cap, the frame average, its clamp and the move) is one
// definition, src/runtime/patch/dronemuzzle_rules.h, which test_dronemuzzle_rules.cpp drives on PC and dronemuzzle_test.cpp
// on Quest. dronefix.cpp hooks the game and links no test binary, so these pin its wiring: TickFrame feeds the shared
// average, MuzzleTransform moves the pose the game read through the shared rule and leaves it when the rule says no, and
// neither caller keeps a copy of the rule.

const (
	dronefix         = "src/runtime/combat/dronefix.cpp"
	questDronemuzzle = "src/quest/combat/dronemuzzle.cpp"
	muzzleRule       = `#include "runtime/patch/dronemuzzle_rules.h"`
)

func TestTickFrameFeedsTheSharedFrameAverage(t *testing.T) {
	t.Parallel()
	src := code(t, dronefix)
	if !strings.Contains(src, muzzleRule) {
		t.Errorf("%s no longer includes the shared muzzle rule", dronefix)
	}
	tick := body(t, src, "void TickFrame()")
	if !strings.Contains(tick, "g_frame_dt = dm::FrameDt(g_frame_dt, dt);") {
		t.Errorf("TickFrame no longer feeds the frame time to dm::FrameDt:\n%s", tick)
	}
}

func TestMuzzleTransformMovesThePoseItReadThroughTheSharedRule(t *testing.T) {
	t.Parallel()
	fn := body(t, code(t, dronefix), "Pose *__fastcall MuzzleTransform(")
	at := func(s string) int {
		i := strings.Index(fn, s)
		if i < 0 {
			t.Errorf("MuzzleTransform no longer has %q:\n%s", s, fn)
		}
		return i
	}
	read := at("Real_TransformGet(transforms, out, index);")
	root := at("const float root[3] = { out->p[0], out->p[1], out->p[2] };")
	frame := at("const float frame = g_frame_dt;")
	move := at("if (!dm::MuzzleMove(root, v, frame, out->p)) return out;")
	at("const float dt = dm::ClampDt(frame);")
	if read > root || root > move || frame > move {
		t.Errorf("MuzzleTransform must read the pose, copy its position and the frame time, then move it:\n%s", fn)
	}
}

func TestTheInstallLineReportsTheSharedMuzzle(t *testing.T) {
	t.Parallel()
	if !strings.Contains(code(t, dronefix), "dm::kMuzzle[0], dm::kMuzzle[1], dm::kMuzzle[2]") {
		t.Error("the install line no longer reports dm::kMuzzle")
	}
}

func TestNeitherCallerKeepsACopyOfTheRule(t *testing.T) {
	t.Parallel()
	pc := code(t, dronefix)
	for _, literal := range []string{"MUZZLE[", "BACK_MAX", "0.075634f", "1.0f / 240.0f", "1.0f / 120.0f", "1.0f / 30.0f", "len < 1e-4f", "xl > 1e-4f"} {
		if strings.Contains(pc, literal) {
			t.Errorf("%s holds %q, which belongs to src/runtime/patch/dronemuzzle_rules.h", dronefix, literal)
		}
	}
	quest := code(t, questDronemuzzle)
	if !strings.Contains(quest, muzzleRule) {
		t.Errorf("%s no longer includes the shared muzzle rule", questDronemuzzle)
	}
	if regexp.MustCompile(`\bkMuzzle\[3\]\s*=`).MatchString(quest) {
		t.Errorf("%s defines a muzzle of its own", questDronemuzzle)
	}
	if _, err := os.Stat(filepath.Join(root(t), "src/quest/combat/dronemuzzle_core.h")); err == nil {
		t.Error("src/quest/combat/dronemuzzle_core.h is back: the Quest's rule is the shared header")
	}
}
