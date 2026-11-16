package main

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"math"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
)

// MEDIGUN's data records (#2242, PLAN-v5 piece G-MED). Cat's tools/fix_medigun.py at 82ad7ba builds MEDIGUN and ships it
// with MEDIGUN_SHOWN = False (:45, :284-286): the gun is NOT listed in gear_table.weapons, so no loadout terminal offers it.
// That is why this file has no equipment step: equipmentAdds (steps_equipment.go) names no medigun, and
// TestNoStepListsMedigunOnATerminal pins it. The two records that are data, and that his tool writes whether or not the
// terminal shows the gun, are:
//
//  1. mp_weapon_settings: ability_properties.medigun, the tuning his script reads (fix_medigun.py:174-190 `tuning`, written at
//     :292), computed from the stock Repair Matrix row ability_properties.heal and his config section "medigun"
//     (config/mp_weapon_settings.json:91-110).
//  2. The global level's heal-bubble table (CR15NetHealBubbleCR, 1ad01a2a91709a1a): a copy of each combat player's stock
//     heal-bubble row under the MEDIGUN bubble handle (fix_medigun.py:224-238, :312-315).
//
// Not here: the MEDIGUN script's rekey and its CScriptCR attach row (G-MED-ATTACH) and the .mbeam script change (H-MED).
//
// Both rules read the stock record, which is where his tool starts too: S-BASE section 1 puts mp_weapon_settings's
// ability_properties.heal and the global bubble table in the group "equal to stock" (the gun patch differs from stock in none
// of them). The step checks the stock values it relies on first and refuses a record that differs.
//
// Declared fields (Step.Writes, what the stage82 oracle compares; nothing else of either record is written):
//   - mp_weapon_settings 607d858c90268f9a/11a8523ed4724ca7: the object ability_properties.medigun, added; every other key kept
//     (readMedigunTuning). The run-time balance merge (src/core/combat_balance/balance_values.h) writes the top-level
//     "medigun" object of the same values, never ability_properties.medigun, so no field is written by both (ADR 0039).
//     steps_dot_preset.go and steps_point.go write other keys of the same resource.
//   - 1ad01a2a91709a1a/3f9915d3001dc28e: the rows of the MEDIGUN bubble, appended, each a copy of a stock heal-bubble row
//     with +0 set to mediBubble (readRowsWhere). No stock row changes. steps_buff.go appends the OVERCHARGE bubble's rows
//     to the same table and declares only those, so the two fields share no row.
//
// His `tuning` copies every key of the config section, `_about` and the `_why_*` notes included; those keys are prose that
// his script never reads, and the run-time merge skips `_` keys (balance_merge.h), so they are not written here.
//
// weaponSettingsName (steps_dot_preset.go), healBubbleTable, globalLevel and healBubble (steps_buff.go) and objectEnd
// (steps_point.go) are the landed ones.
const (
	mediBubble   = 0x51ef178e73ccf82f // CSymbol64("echocombat_medigun_bubble"), fix_medigun.py:48
	bubbleRowMin = 16                 // a bubble row's handle at +0 and its actor at +8
)

// mediActors are the actors that carry Repair Matrix's bubble: the two combat player prefabs (fix_smoke.py:75) and the copy
// other machines show a player as (fix_buff.py:89; fix_medigun.py:171 MEDI_ACTORS = OC_ACTORS).
var mediActors = []uint64{0x91d8de0806964a11, 0xf1e70acd0dde0312, 0xb113997837c29003}

// The stock Repair Matrix values the tuning is computed from (S-BASE 2.2: equal to stock, "heal_amt 114.0, heal_duration 12.0").
const (
	stockHealAmt      = 114.0
	stockHealDuration = 12.0
)

// His config section (config/mp_weapon_settings.json:91-110 at 82ad7ba), the values `tuning` reads and copies.
const (
	cfgEnabled       = false
	cfgHealScale     = 0.85
	cfgBeamSeconds   = 15
	cfgOverheat      = 4
	cfgCool          = 5
	cfgConeAngle     = 20
	cfgRange         = 20
	cfgConeSlack     = 5
	cfgHand          = "auto"
	cfgHandOffset    = "0.064,0.000,0.203"
	cfgHandOffsetL   = "0.124,0,0.203"
	cfgBeamBone      = "ToolAttach1"
	cfgAim           = "gun"
	coneTag          = 500    // fix_medigun.py:165: PowerBoost reads the cone from the radius fraction, angle + CONE_TAG
	radiusFraction   = 100000 // fix_medigun.py:185: heal_radius = range + (angle + CONE_TAG) / 100000
	rateDecimals     = 1e4    // fix_medigun.py:183 round(rate, 4)
	healDecimals     = 1e3    // fix_medigun.py:184 round(rate * seconds, 3)
	maxRange         = 100    // fix_medigun.py:179 1 <= length <= 100 (the raise is :180)
	maxConeAngle     = 90     // fix_medigun.py:179 1 <= angle <= 90 (the raise is :180)
	unlimitedHealing = 0
	noHealRamp       = 0
	noSelfHeal       = 0
	minHealMult      = 1
)

// medigunTuning is ability_properties.medigun, in the key order his dict has: the config section, then the computed keys.
type medigunTuning struct {
	Enabled           bool    `json:"enabled"`
	HealScale         float64 `json:"heal_scale"`
	BeamSeconds       int     `json:"beam_seconds"`
	OverheatSeconds   int     `json:"overheat_seconds"`
	CoolSeconds       int     `json:"cool_seconds"`
	ConeAngle         int     `json:"cone_angle"`
	Range             int     `json:"range"`
	ConeSlack         int     `json:"cone_slack"`
	Hand              string  `json:"hand"`
	HandOffset        string  `json:"hand_offset"`
	HandOffsetLeft    string  `json:"hand_offset_left"`
	BeamBone          string  `json:"beam_bone"`
	Aim               string  `json:"aim"`
	HealPerSecond     float64 `json:"heal_per_second"`
	HealAmt           float64 `json:"heal_amt"`
	HealDuration      int     `json:"heal_duration"`
	HealRadius        float64 `json:"heal_radius"`
	CapHealing        bool    `json:"cap_healing"`
	MaxHealing        int     `json:"max_healing"`
	ScaleHealing      bool    `json:"scale_healing"`
	MinHealMult       int     `json:"min_heal_mult"`
	TimeToMaxHeal     int     `json:"time_to_max_heal"`
	SelfHealMult      int     `json:"self_heal_mult"`
	UniqueHealingOnly bool    `json:"unique_healing_only"`
}

// tuning is fix_medigun.py's `tuning(heal, cfg)` over the stock heal row and the config above.
func tuning(healAmt, healDuration float64) (medigunTuning, error) {
	if err := checkCone(cfgRange, cfgConeAngle); err != nil {
		return medigunTuning{}, err
	}
	rate := healAmt / healDuration * cfgHealScale
	return medigunTuning{
		Enabled: cfgEnabled, HealScale: cfgHealScale, BeamSeconds: cfgBeamSeconds, OverheatSeconds: cfgOverheat, CoolSeconds: cfgCool,
		ConeAngle: cfgConeAngle, Range: cfgRange, ConeSlack: cfgConeSlack, Hand: cfgHand, HandOffset: cfgHandOffset,
		HandOffsetLeft: cfgHandOffsetL, BeamBone: cfgBeamBone, Aim: cfgAim,
		HealPerSecond: math.Round(rate*rateDecimals) / rateDecimals,
		HealAmt:       math.Round(rate*cfgBeamSeconds*healDecimals) / healDecimals,
		HealDuration:  cfgBeamSeconds,
		HealRadius:    float64(cfgRange) + float64(cfgConeAngle+coneTag)/radiusFraction,
		CapHealing:    false, MaxHealing: unlimitedHealing, ScaleHealing: false, MinHealMult: minHealMult,
		TimeToMaxHeal: noHealRamp, SelfHealMult: noSelfHeal, UniqueHealingOnly: true,
	}, nil
}

// checkCone is his limit on the beam's cone (fix_medigun.py:179-180): range in 1..100 and cone_angle in 1..90.
func checkCone(length, angle int) error {
	if length < 1 || length > maxRange || angle < 1 || angle > maxConeAngle {
		return fmt.Errorf("range %d / cone_angle %d out of range", length, angle)
	}
	return nil
}

const (
	medigunSettingsStep = "weapon settings get ability_properties.medigun"
	medigunBubblesStep  = "heal-bubble table gets a MEDIGUN bubble per player actor"
)

func init() {
	steps = append(steps,
		Step{Name: medigunSettingsStep, Apply: medigunSettings,
			Writes: []Field{{Name: "ability_properties.medigun", Set: Sets[0], Type: jsonResourceType,
				Records: []uint64{weaponSettingsName}, Read: readMedigunTuning}}},
		Step{Name: medigunBubblesStep, Apply: medigunBubbles,
			Writes: []Field{{Name: "heal bubble rows of the MEDIGUN bubble", Set: Sets[0], Type: healBubbleTable,
				Records: []uint64{globalLevel}, Read: readRowsWhere(0, mediBubble)}}},
	)
}

// readMedigunTuning is a reader of ability_properties.medigun of a settings resource, as the object re-encoded with its
// keys sorted, so two builds that lay the record out differently compare by value. A value that is not an object is
// refused.
func readMedigunTuning(b []byte) ([]byte, error) {
	if len(b) < jsonHeader || binary.LittleEndian.Uint64(b[jsonLengthOff:]) != uint64(len(b)-jsonHeader) {
		return nil, fmt.Errorf("not a JSON resource of %d bytes (a 0x220-byte header whose +8 is the body's length)", len(b))
	}
	body := b[jsonHeader:]
	start, end, err := valueSpan(body, "ability_properties", "medigun")
	if err != nil {
		return nil, err
	}
	var v map[string]any
	if err := json.Unmarshal(body[start:end], &v); err != nil || v == nil {
		return nil, fmt.Errorf("ability_properties.medigun is not an object: %v", err)
	}
	return json.Marshal(v)
}

func medigunSettings(p *Pack) error {
	b, err := p.Read(Sets[0], jsonResourceType, weaponSettingsName)
	if err != nil {
		return err
	}
	out, err := addMedigunTuning(b)
	if err != nil {
		return fmt.Errorf("mp_weapon_settings: %w", err)
	}
	return p.Put(Sets[0], jsonResourceType, weaponSettingsName, out)
}

// addMedigunTuning is the resource with ability_properties.medigun added, the header kept and its length set. It refuses a
// resource whose Repair Matrix row is not the stock one, or that already has a medigun row.
func addMedigunTuning(b []byte) ([]byte, error) {
	if len(b) < jsonHeader || binary.LittleEndian.Uint64(b[jsonLengthOff:]) != uint64(len(b)-jsonHeader) {
		return nil, fmt.Errorf("not a JSON resource of %d bytes (a 0x220-byte header whose +8 is the body's length)", len(b))
	}
	body := b[jsonHeader:]
	var doc struct {
		Ability *struct {
			Heal *struct {
				HealAmt      *float64 `json:"heal_amt"`
				HealDuration *float64 `json:"heal_duration"`
			} `json:"heal"`
			Medigun json.RawMessage `json:"medigun"`
		} `json:"ability_properties"`
	}
	if err := json.Unmarshal(body, &doc); err != nil {
		return nil, err
	}
	if doc.Ability == nil {
		return nil, fmt.Errorf("no ability_properties object")
	}
	heal := doc.Ability.Heal
	if heal == nil || heal.HealAmt == nil || heal.HealDuration == nil {
		return nil, fmt.Errorf("ability_properties.heal has no heal_amt and heal_duration")
	}
	if *heal.HealAmt != stockHealAmt || *heal.HealDuration != stockHealDuration {
		return nil, fmt.Errorf("ability_properties.heal is heal_amt %v heal_duration %v, the stock row's are %v and %v",
			*heal.HealAmt, *heal.HealDuration, stockHealAmt, stockHealDuration)
	}
	if doc.Ability.Medigun != nil {
		return nil, fmt.Errorf("ability_properties already has a medigun row")
	}
	t, err := tuning(*heal.HealAmt, *heal.HealDuration)
	if err != nil {
		return nil, err
	}
	text, err := json.Marshal(t)
	if err != nil {
		return nil, err
	}
	// ability_properties holds heal (checked above), so the new member follows a comma.
	end, _, err := objectEnd(body, "ability_properties")
	if err != nil {
		return nil, err
	}
	var add bytes.Buffer
	add.WriteString(`,"medigun":`)
	add.Write(text)
	nb := append(append(append([]byte(nil), body[:end]...), add.Bytes()...), body[end:]...)
	out := append([]byte(nil), b[:jsonHeader]...)
	binary.LittleEndian.PutUint64(out[jsonLengthOff:], uint64(len(nb)))
	return append(out, nb...), nil
}

func medigunBubbles(p *Pack) error {
	b, err := p.Read(Sets[0], healBubbleTable, globalLevel)
	if err != nil {
		return err
	}
	t, err := radpack.ParseRowTable(b)
	if err != nil {
		return fmt.Errorf("heal-bubble table: %w", err)
	}
	if err := copyBubbles(t); err != nil {
		return fmt.Errorf("heal-bubble table: %w", err)
	}
	return p.Put(Sets[0], healBubbleTable, globalLevel, t.Bytes())
}

// copyBubbles appends, for each stock heal-bubble row of an actor in mediActors, a copy with the handle at +0 set to
// mediBubble, in the stock table's row order (fix_medigun.py:224-238: his loop walks the stock rows at :230-232 and
// ct.add_element appends each copy at :235). The stock table has exactly one heal-bubble row for each of the three actors
// (measured on the shipped table), so each is checked before anything is added.
func copyBubbles(t *radpack.RowTable) error {
	if t.Size < bubbleRowMin {
		return fmt.Errorf("rows of %d bytes end before the actor at +%#x", t.Size, keyOff)
	}
	stock := map[uint64]int{}
	for i, r := range t.Rows {
		switch radpack.U64(r, 0) {
		case mediBubble:
			return fmt.Errorf("the table already has a MEDIGUN bubble row (actor %016x)", radpack.U64(r, keyOff))
		case healBubble:
			if _, dup := stock[radpack.U64(r, keyOff)]; dup {
				return fmt.Errorf("two heal-bubble rows for actor %016x", radpack.U64(r, keyOff))
			}
			stock[radpack.U64(r, keyOff)] = i
		}
	}
	for _, actor := range mediActors {
		if _, ok := stock[actor]; !ok {
			return fmt.Errorf("no heal-bubble row %016x for actor %016x", uint64(healBubble), actor)
		}
	}
	medi := map[uint64]bool{}
	for _, actor := range mediActors {
		medi[actor] = true
	}
	stockRows := t.Rows // the copies are appended to t.Rows; only the stock rows are walked
	for _, r := range stockRows {
		if radpack.U64(r, 0) != healBubble || !medi[radpack.U64(r, keyOff)] {
			continue
		}
		c := append([]byte(nil), r...)
		radpack.SetU64(c, 0, mediBubble)
		t.Rows = append(t.Rows, c)
	}
	return nil
}
