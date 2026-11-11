package combatwiring

import (
	"regexp"
	"strings"
	"testing"
)

// #2359: the drone kill rules (the symbols, the chase velocity, the gun table, the group table) are one header,
// src/runtime/patch/dronekill_rules.h, which test_dronekill_rules.cpp drives on PC and dronekill_test.cpp on Quest.
// dronefix.cpp hooks the game and links no test binary, so these pin its wiring: each PC hook takes its rule from the
// header, neither port keeps a copy of the table, the values or the chase arithmetic, and the calls the rules add sit
// only in the functions the arena never reaches.

const dronekillRule = `#include "runtime/patch/dronekill_rules.h"`

// dronefixCopies are the text of the rules as they were before the header, and the symbol values, which the port must no
// longer spell. The match is case-insensitive for the symbols.
var dronefixCopies = []string{
	"struct DroneGroup", "g_drones[", "g_pending[", "g_npending", "struct GunState", "GunState g_guns",
	"DEAD_SWALLOW_MS", "CHASE_SPEED", "CHASE_DISTANCE", "CHASE_GAIN", "CHASE_ABOVE", "dist < 1e-3f",
	">= 3000", "< 5000", "== 16", "bool InDeadGroup(", "DroneGroup *Newest(", "void AddComp(",
}

var dronekillSymbols = []string{
	"10e40287a9bf5a13", "c9dc08cfc2f5920f", "2e40131422f7b1fc", "38ee951a26fb816a",
	"72f91fffa17c3c8c", "106c63a2927b7a4d", "45b09f0fa9f7fa33",
}

// topLevelDef matches a line that opens a top-level function definition: a column-0 line ending in "{" with a "(".
var topLevelDef = regexp.MustCompile(`(?m)^[A-Za-z_][^\n]*\([^\n]*\) \{[ \t]*$`)

// enclosingDef returns the top-level definition line that contains byte offset at, or "" when there is none.
func enclosingDef(src string, at int) string {
	found := ""
	for _, loc := range topLevelDef.FindAllStringIndex(src, -1) {
		if loc[0] > at {
			break
		}
		found = src[loc[0]:loc[1]]
	}
	return found
}

func TestDronefixIncludesTheDroneKillRules(t *testing.T) {
	t.Parallel()
	src := code(t, dronefix)
	if !strings.Contains(src, dronekillRule) {
		t.Errorf("%s does not include %s", dronefix, dronekillRule)
	}
	if !strings.Contains(src, "namespace dk = nevr_dronekill;") {
		t.Errorf("%s does not alias the rules as dk", dronefix)
	}
}

func TestHookedEnableCompJoinsSwallowsAndOpensGroupsThroughTheRules(t *testing.T) {
	t.Parallel()
	src := code(t, dronefix)
	fn := body(t, src, "void __fastcall Hooked_EnableComp(")
	release := "g_groups.Release(SysKey(system), index, actor, 0, now)"
	enable := "g_groups.Enable(SysKey(system), index, actor, 0, now, what == 1).swallowed"
	for _, want := range []string{release, enable} {
		if !strings.Contains(fn, want) {
			t.Errorf("%s: Hooked_EnableComp does not call %s (the space is 0: PC reads no game space at an enable, D10)", dronefix, want)
		}
	}
	last := max(strings.Index(fn, release), strings.Index(fn, enable))
	if j := strings.Index(fn, "if (!swallow) Real_EnableComp("); j < 0 || j < last {
		t.Errorf("%s: Hooked_EnableComp does not pass a swallowed enable to the game only after the rule decides", dronefix)
	}
}

func TestShutdownKillsAndHidesThroughTheRules(t *testing.T) {
	t.Parallel()
	src := code(t, dronefix)
	fn := body(t, src, "bool Shutdown(")
	for _, want := range []string{"g_groups.MarkDead(SysKey(dcs), didx)", "g_groups.KillIndex(didx)", "dk::HideGunOnKill(g_guns);"} {
		if !strings.Contains(fn, want) {
			t.Errorf("%s: Shutdown does not call %s", dronefix, want)
		}
	}
	if strings.Contains(fn, "SetGun(") {
		t.Errorf("%s: Shutdown still hides the gun through SetGun, not the shared table (D4)", dronefix)
	}
}

func TestExpiredDamageableMarksItsGroupDead(t *testing.T) {
	t.Parallel()
	src := code(t, dronefix)
	fn := body(t, src, "void ExpiredDamageable(")
	if !strings.Contains(fn, "g_groups.MarkDead(SysKey(system), index)") {
		t.Errorf("%s: ExpiredDamageable does not mark its group dead through the shared rule (D11)", dronefix)
	}
}

func TestHookedDeathShutsDownOnlyALiveDrone(t *testing.T) {
	t.Parallel()
	src := code(t, dronefix)
	fn := body(t, src, "void __fastcall Hooked_Death(")
	if !strings.Contains(fn, "if (!Shutdown(") {
		t.Errorf("%s: Hooked_Death does not return unless Shutdown found a live drone", dronefix)
	}
}

func TestAddTouchJoinsTheNewestGroupThroughTheRules(t *testing.T) {
	t.Parallel()
	src := code(t, dronefix)
	fn := body(t, src, "void AddTouch(")
	for _, want := range []string{"g_groups.Newest(actor, now)", "g_groups.Add("} {
		if !strings.Contains(fn, want) {
			t.Errorf("%s: AddTouch does not call %s", dronefix, want)
		}
	}
}

func TestTouchLiveAsksTheSharedTable(t *testing.T) {
	t.Parallel()
	src := code(t, dronefix)
	if fn := body(t, src, "bool TouchLive("); !strings.Contains(fn, "g_groups.LiveComp(SysKey(system), index)") {
		t.Errorf("%s: TouchLive does not ask the shared table whether the component is live", dronefix)
	}
}

func TestChaseTargetComputesItsVelocityThroughTheRule(t *testing.T) {
	t.Parallel()
	src := code(t, dronefix)
	fn := body(t, src, "void ChaseTarget(")
	if !strings.Contains(fn, "dk::ChaseVelocity(me.p, them.p, v, &dist, &speed)") {
		t.Errorf("%s: ChaseTarget does not compute its velocity through dk::ChaseVelocity", dronefix)
	}
	if strings.Contains(fn, "sqrtf") {
		t.Errorf("%s: ChaseTarget still computes a distance of its own (sqrtf)", dronefix)
	}
}

func TestTheGunTableIsTheSharedOneKeyedByActorSymbol(t *testing.T) {
	t.Parallel()
	src := code(t, dronefix)
	if fn := body(t, src, "void SetGun("); !strings.Contains(fn, "g_guns.Set(actor, shown)") {
		t.Errorf("%s: SetGun does not write the shared gun table", dronefix)
	}
	if fn := body(t, src, "bool GunShown("); !strings.Contains(fn, "g_guns.Shown(actor, shown)") {
		t.Errorf("%s: GunShown does not read the shared gun table", dronefix)
	}
	// D4: the gun table is keyed by the node's actor symbol, the one both callers pass
	fn := body(t, src, "void __fastcall Hooked_EnableEnter(")
	for _, want := range []string{"GunShown(data[0], &shown)", "SetGun(data[0], (uint32_t)data[4])"} {
		if !strings.Contains(fn, want) {
			t.Errorf("%s: Hooked_EnableEnter does not key the gun by the actor symbol: missing %s", dronefix, want)
		}
	}
}

func TestDronefixKeepsNoCopyOfTheRules(t *testing.T) {
	t.Parallel()
	src := code(t, dronefix)
	for _, copied := range dronefixCopies {
		if strings.Contains(src, copied) {
			t.Errorf("%s still spells %q, which the shared header now owns", dronefix, copied)
		}
	}
	lower := strings.ToLower(src)
	for _, sym := range dronekillSymbols {
		if strings.Contains(lower, sym) {
			t.Errorf("%s still spells the symbol 0x%s, which the shared header now owns", dronefix, strings.ToUpper(sym))
		}
	}
}

// The arena statement (design section 4): the rules add no call on any path the arena reaches. Each call is in the one
// block that calls it today, and each function that touches the shared tables is one the drone path already used.
func TestTheDroneRulesAreCalledOnlyFromTheDronePaths(t *testing.T) {
	t.Parallel()
	src := code(t, dronefix)
	inst := body(t, src, "bool Install()")
	gate := body(t, inst, "if (g_drone_on) {")
	for _, call := range []string{"InstallDeathRule(base);", "InstallChase(base);", "InstallEnableMirror(base);", "InstallGrabRule(base);"} {
		if strings.Count(inst, call) != 1 || !strings.Contains(gate, call) {
			t.Errorf("%s: Install calls %s outside its if (g_drone_on) block", dronefix, call)
		}
	}
	follow := body(t, src, "void __fastcall Hooked_FollowUpdate(")
	fgate := body(t, follow, "if (g_drone_on) {")
	for _, call := range []string{"DroneHitsDue();", "KillTestDue();"} {
		if strings.Count(follow, call) != strings.Count(fgate, call) || strings.Count(fgate, call) == 0 {
			t.Errorf("%s: Hooked_FollowUpdate calls %s outside its if (g_drone_on) block", dronefix, call)
		}
	}
	allowed := []string{
		"Hooked_EnableComp", "AddTouch", "Shutdown", "ExpiredDamageable", "KillTestDue", "SetGun", "GunShown",
		"TouchLive", "Hooked_WeaponHit", "DroneHitsDue",
	}
	for _, tbl := range []string{"g_groups.", "g_guns."} {
		for at := 0; ; {
			i := strings.Index(src[at:], tbl)
			if i < 0 {
				break
			}
			at += i
			def := enclosingDef(src, at)
			ok := false
			for _, name := range allowed {
				if strings.Contains(def, name+"(") {
					ok = true
				}
			}
			if !ok {
				t.Errorf("%s: %s is used in %q, which is not a drone path", dronefix, tbl, def)
			}
			at += len(tbl)
		}
	}
}

const (
	questDronekill     = "src/quest/combat/dronekill.cpp"
	questDronekillCore = "src/quest/combat/dronekill_core.h"
)

// The Quest's wiring to the same header (#2359): the core and its caller use the shared rules, the caller keys the gun
// table by the node's actor symbol, and neither file keeps a copy of the table, the gun table, the symbols or the chase.
func TestQuestDronekillUsesTheSharedHeader(t *testing.T) {
	t.Parallel()
	if core := code(t, questDronekillCore); !strings.Contains(core, dronekillRule) {
		t.Errorf("%s does not include %s", questDronekillCore, dronekillRule)
	}
	src := code(t, questDronekill)
	for _, want := range []string{"ChaseVelocity(", "g_guns.Set(d.actor, d.enable)", "g_guns.Shown(d.actor, &shown)", "HideGunOnKill(g_guns)"} {
		if !strings.Contains(src, want) {
			t.Errorf("%s does not use the shared rule: missing %s", questDronekill, want)
		}
	}
	for _, gone := range []string{"g_guns.Set(cap.actor", "g_guns.Shown(cap.actor", "g_guns.Set(g->actor"} {
		if strings.Contains(src, gone) {
			t.Errorf("%s still keys the gun table by the captured actor: %s (D4)", questDronekill, gone)
		}
	}
}

func TestQuestDronekillKeepsNoCopyOfTheRules(t *testing.T) {
	t.Parallel()
	core := code(t, questDronekillCore)
	for _, copied := range []string{
		"struct GunTable", "inline bool ChaseVelocity", "kChaseSpeed =", "kJoinMs =", "kPendingMs =", "kSwallowMs =",
		"kPending =", "dist2 < 1e-6f", "g.number > best->number", "memmove(pending",
	} {
		if strings.Contains(core, copied) {
			t.Errorf("%s still spells %q, which the shared header now owns", questDronekillCore, copied)
		}
	}
	lower := strings.ToLower(core)
	for _, sym := range dronekillSymbols {
		if strings.Contains(lower, sym) {
			t.Errorf("%s still spells the symbol 0x%s, which the shared header now owns", questDronekillCore, strings.ToUpper(sym))
		}
	}
}

// The arena statement on the Quest: each of the three hooks returns before the drone work when no combat map is loaded.
func TestQuestDronekillArenaCallsStayBehindTheScopeGate(t *testing.T) {
	t.Parallel()
	src := code(t, questDronekill)
	enter := body(t, src, "void EnterHandler(")
	if i, j := strings.Index(enter, "!g_inScope"), strings.Index(enter, "EnterInScope("); i < 0 || j < i {
		t.Errorf("%s: EnterHandler does not return outside a combat map before EnterInScope", questDronekill)
	}
	attack := body(t, src, "void AttackHandler(")
	if i, j := strings.Index(attack, "!g_inScope"), strings.Index(attack, "AttackInScope("); i < 0 || j < i {
		t.Errorf("%s: AttackHandler does not return outside a combat map before AttackInScope", questDronekill)
	}
	killed := body(t, src, "void OnKilled(")
	if head := killed[:min(len(killed), 160)]; !strings.Contains(head, "!g_inScope") || !strings.Contains(head, "return") {
		t.Errorf("%s: OnKilled does not return first outside a combat map", questDronekill)
	}
}

// D11 (#2359): the drone's end is the table's dead-marking call, and it sits in the branch that is taken when the node turns the damageable off
// (the branch is not taken when the node turns it on, which is the release).
func TestQuestDronekillD11MarksTheGroupDeadWhenTheDamageableGoesOff(t *testing.T) {
	t.Parallel()
	src := code(t, questDronekill)
	enter := body(t, src, "void EnterInScope(")
	branch := body(t, enter, "if (!d.enable && d.system == kDamageable) {")
	if !strings.Contains(branch, "g_table.Off(") {
		t.Errorf("%s: EnterInScope does not mark the drone's group dead in the branch taken when its damageable goes off (D11)", questDronekill)
	}
}

// The PC port's drone end and handle lines (#2359, D11), which no test compiles: dronefix.cpp hooks the game. Each pin names the
// function that holds the line and counts it there, so a line moved to another function, or removed, fails.
func TestShutdownTakesOnlyALiveGroupAndKeepsTheComponentHandles(t *testing.T) {
	t.Parallel()
	src := code(t, dronefix)
	fn := body(t, src, "bool Shutdown(")
	for _, want := range []string{
		"if (m.wasAlive) g = m.group;",
		"g_last_death = GetTickCount64();",
		"comps[i] = { SysPtr(d.comps[i].system), d.comps[i].handle };",
	} {
		if strings.Count(fn, want) != 1 {
			t.Errorf("%s: Shutdown must contain %q once (a repeat end does not shut the drone down again, and each component keeps its handle)", dronefix, want)
		}
	}
}

func TestHookedEnableCompStampsTheDeploy(t *testing.T) {
	t.Parallel()
	src := code(t, dronefix)
	fn := body(t, src, "void __fastcall Hooked_EnableComp(")
	if strings.Count(fn, "g_last_deploy = now;") != 1 {
		t.Errorf("%s: Hooked_EnableComp must stamp g_last_deploy with now once (the carried-drone line reads it)", dronefix)
	}
}

func TestHookedWeaponHitMatchesTheDamageableByItsLowIndexBits(t *testing.T) {
	t.Parallel()
	src := code(t, dronefix)
	fn := body(t, src, "void __fastcall Hooked_WeaponHit(")
	if !strings.Contains(fn, "(g.handle & 0xffff)") {
		t.Errorf("%s: Hooked_WeaponHit does not match a group's damageable by the low 16 bits of its handle", dronefix)
	}
}
