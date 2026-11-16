package main

import (
	"fmt"
	"slices"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
	"github.com/EchoTools/nevr-runtime/tools/internal/scriptgate"
)

// The DOT FIELD's remote trigger script, attached to the DOT magazine (#2242, PLAN-v7 10.1 G-DOTT-ATTACH; he_is_the_cat's
// tools/fix_dot_trigger.py at 82ad7ba). The DOT script (97d0806e6ea89130) activates the field when its remote trigger is
// pulled, but nothing calls its activation and its four companion functions come from a script that was never shipped.
// His tool gives the field a third script: a copy of the detonator script (190ed14fe3da5ed1) under the name
// mp_combat_grenade_dot_trigger (4e8af51fe7d6af8c), which the DOT magazine runs. The copy's behaviour is H-DOTT's hooks
// (PLAN-v7 4.3), not this step's: the copy is stock bytes, listed as a rekey, and its edits are not made here.
//
// The step does three things, in order, and refuses before it writes anything:
//
//  1. CScriptCRWin10 (scriptTable, the same table steps_drone.go changes): in each of droneLevels, a row that runs the
//     copy for the DOT magazine's actor is inserted right after the DOT field's own row (fix_dot_trigger.py:158-169). The
//     row is a copy of the detonator script's row with its script ids at +0 and +32 and its actor at +8 set to the
//     copy and the DOT magazine (:166-168). The rows are 720 bytes (fix_dot_trigger.py:118, the stride the PLAN states at
//     10.3). The stock table has no trigger row, and it has the DOT field's row and a detonator row whose pool slots at
//     +48, +104, ..., +664 hold nothing (fix_dot_trigger.py:163-164 refuses a donor that does; a copy would share it).
//  2. CScriptResourceWin10 (991421b41de58370): the copy's entry is four zero bytes (fix_dot_trigger.py:64, 190), so the
//     level resolves the script through it before it loads the DLL. The stock set must not list one for the copy.
//  3. The copy itself is listed with Pack.Rekey (the stock detonator's sha-256 in the label's scripts list, PLAN-v7 4.2);
//     no script byte is written.
//
// Declared fields (Step.Writes, what the stage82 oracle compares for this step): the copy's rows in each level's script
// table, and its CScriptResourceWin10 entry. Every other row and byte is the stock one.
//
// Deliberate difference from his tool: his attach returns the table unchanged when the copy is already in it (a second
// run is a no-op, fix_dot_trigger.py:156-157). This step refuses that table, as droneScriptRow refuses a second run, so a
// stock the step was already applied to is named, not silently accepted.
//
// Base: none. The DOT magazine's actor and its script are the stock table's, and his tool reads no gun-patch file
// (PLAN-v7 4.4, S-BASE: the DOT trigger is not a gun patch row); the gun patch's scriptTable files do not hold this row,
// so the step is PastGunPatch (the gun patch comparison runs before it).
const (
	dotTriggerStepName = "DOT FIELD remote trigger script attached to the DOT magazine"
	// The ids are fix_dot_trigger.py:73-75 and 17: the detonator script, the DOT field's own script, the copy
	// (CSymbol64 "mp_combat_grenade_dot_trigger"), and the DOT magazine actor (dotMagazineActor, steps_dot_stick.go).
	dotDetonatorScript = 0x190ed14fe3da5ed1
	dotFieldScript     = 0x97d0806e6ea89130
	dotTriggerScript   = 0x4e8af51fe7d6af8c
	// Each script's entry is this many zero bytes (fix_dot_trigger.py:64, SCRIPT_RESOURCE_BODY = bytes(4)).
	scriptResourceEntry = 4
	// A CScriptCR row's script at +0 (steps_drone.go:18-19). The row size (scriptgate.RowStride, fix_dot_trigger.py:118), the
	// +32 script (scriptgate.ScriptOffset) and the CScriptResourceWin10 type (scriptgate.ScriptType, fix_dot_trigger.py:61)
	// are scriptgate's, the one home of these facts.
	scriptNameOff = 0x00
)

// scriptPoolOffs are the donor row's pool slots (fix_dot_trigger.py:163): the u64 at each one's +8 must be zero.
var scriptPoolOffs = []int{48, 104, 160, 216, 272, 328, 384, 440, 496, 552, 608, 664}

// dotTriggerName is the copy's name as a 16-hex script name (Pack.Rekey takes the spelling).
var dotTriggerName = fmt.Sprintf("%016x", uint64(dotTriggerScript))

// dotDetonatorName is the stock script the copy is made from, as a 16-hex name.
var dotDetonatorName = fmt.Sprintf("%016x", uint64(dotDetonatorScript))

func init() {
	// After droneScriptStep: Cat's tool runs after fix_drone_script.py and stacks onto its table
	// (he_is_the_cat tools/deploy_and_launch.py:352, "after fix_drone_script: stacks on its table"), so the copy takes
	// its row after the drone's in each level table, the order the pipeline gives. The gated tie test shows the bytes
	// do not depend on the order.
	// Before mapTermStepName: Cat's fix_dot_trigger.py runs before fix_map_terminal.py (deploy_and_launch.py:352, :378),
	// and the map terminal writes script tables of map levels this step also writes. Declared here, so the map terminal
	// file is not edited by this piece.
	steps = append(steps, Step{Name: dotTriggerStepName, Apply: attachDotTrigger,
		PastGunPatch: true, After: []string{droneScriptStep}, Before: []string{mapTermStepName},
		Writes: []Field{
			{Name: "CScriptCR rows that run the DOT trigger script for the DOT magazine, the copy's row after the DOT row (none in stock)",
				Set: Sets[0], Type: scriptTable, Records: droneLevels, Read: readDotTriggerRows},
			{Name: "CScriptResourceWin10 entry of the DOT trigger script, four zero bytes", Set: Sets[0], Type: scriptgate.ScriptType,
				Records: []uint64{dotTriggerScript}, Read: readScriptResourceEntry},
		}})
}

// attachDotTrigger changes the five level tables and the entry, then lists the copy. It checks every table and the stock
// entry before it writes, so a refusal leaves the pack as it was.
func attachDotTrigger(p *Pack) error {
	scriptTab := uint64(scriptTable)
	edits := make(map[uint64][]byte, len(droneLevels))
	for _, lvl := range droneLevels {
		b, err := p.Read(Sets[0], int64(scriptTab), int64(lvl))
		if err != nil {
			return err
		}
		t, err := radpack.ParseRowTable(b)
		if err != nil {
			return fmt.Errorf("level %016x: %w", lvl, err)
		}
		if err := insertDotTriggerRow(t); err != nil {
			return fmt.Errorf("level %016x: %w", lvl, err)
		}
		edits[lvl] = t.Bytes()
	}
	entry := uint64(scriptgate.ScriptType)
	if _, found, err := p.sets[Sets[0]].Read(int64(entry), int64(dotTriggerScript)); err != nil {
		return err
	} else if found {
		return fmt.Errorf("the stock set already lists the CScriptResourceWin10 entry of %s; the trigger copy would replace it", dotTriggerName)
	}
	if err := p.Rekey(dotTriggerName, dotDetonatorName); err != nil {
		return err
	}
	for _, lvl := range droneLevels {
		if err := p.Put(Sets[0], int64(scriptTab), int64(lvl), edits[lvl]); err != nil {
			return err
		}
	}
	return p.Put(Sets[0], int64(entry), int64(dotTriggerScript), make([]byte, scriptResourceEntry))
}

// dotTriggerAfter is the index the copy takes in a CScriptCR table, given the index of the DOT field's own row: directly
// after it, before every other row. This is the rule the step follows (fix_dot_trigger.py:169, entries.insert(own[0] + 1,
// ...)); the build known to work in game has the copy there, and the engine may read a level's script rows in order, so
// the position is stated here and not left to the order the steps run in. TestTheDotTriggerRowSitsDirectlyAfterTheDotRow
// asserts it, and that the drone step's row goes the same place whichever runs first.
func dotTriggerAfter(own int) int { return own + 1 }

// insertDotTriggerRow adds the copy's row after the DOT magazine's row of one CScriptCR table, after checking the table
// is the stock one: rows of the shipped size, the copy not in it yet, exactly one DOT field row for the DOT magazine,
// and a detonator row to copy whose pool slots are empty.
func insertDotTriggerRow(t *radpack.RowTable) error {
	if t.Size != scriptgate.RowStride {
		return fmt.Errorf("script rows of %d bytes, want %d (fix_dot_trigger.py:118)", t.Size, scriptgate.RowStride)
	}
	var own []int
	donor := -1
	for i, r := range t.Rows {
		if radpack.U64(r, keyOff) == dotMagazineActor && radpack.U64(r, scriptgate.ScriptOffset) == dotTriggerScript {
			return fmt.Errorf("the table already has the DOT trigger row (%s) for the DOT magazine", dotTriggerName)
		}
		if radpack.U64(r, keyOff) == dotMagazineActor && radpack.U64(r, scriptgate.ScriptOffset) == dotFieldScript {
			own = append(own, i)
		}
		if donor < 0 && radpack.U64(r, scriptNameOff) == dotDetonatorScript && radpack.U64(r, scriptgate.ScriptOffset) == dotDetonatorScript {
			donor = i
		}
	}
	if len(own) != 1 {
		return fmt.Errorf("the table has %d rows of the DOT field script %016x for the DOT magazine, want exactly 1", len(own), uint64(dotFieldScript))
	}
	if donor < 0 {
		return fmt.Errorf("no row of the detonator script %s to copy", dotDetonatorName)
	}
	for _, off := range scriptPoolOffs {
		if v := radpack.U64(t.Rows[donor], off+keyOff); v != 0 {
			return fmt.Errorf("the detonator row carries pool data (%#x at +%#x); a copy would share it (fix_dot_trigger.py:163)", v, off+keyOff)
		}
	}
	nr := append([]byte(nil), t.Rows[donor]...)
	radpack.SetU64(nr, scriptNameOff, dotTriggerScript)
	radpack.SetU64(nr, keyOff, dotMagazineActor)
	radpack.SetU64(nr, scriptgate.ScriptOffset, dotTriggerScript)
	t.Rows = slices.Insert(t.Rows, dotTriggerAfter(own[0]), nr)
	return nil
}

// readDotTriggerRows is the step's first declared field: the rows that run the trigger copy for the DOT magazine, in
// table order (none in the stock table, the copy's row once the step has run).
func readDotTriggerRows(b []byte) ([]byte, error) {
	t, err := radpack.ParseRowTable(b)
	if err != nil {
		return nil, err
	}
	if t.Size < scriptgate.ScriptOffset+8 {
		return nil, fmt.Errorf("script rows of %d bytes end before the second script id at +%#x", t.Size, scriptgate.ScriptOffset)
	}
	var out []byte
	for _, r := range t.Rows {
		if radpack.U64(r, scriptNameOff) == dotTriggerScript && radpack.U64(r, keyOff) == dotMagazineActor {
			out = append(out, r...)
		}
	}
	return out, nil
}

// readScriptResourceEntry is the step's second declared field: the copy's entry, which is the whole record.
func readScriptResourceEntry(b []byte) ([]byte, error) {
	if len(b) != scriptResourceEntry {
		return nil, fmt.Errorf("script entry of %d bytes, want %d", len(b), scriptResourceEntry)
	}
	return b, nil
}
