package main

import (
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
)

// The EchoCombat steps against the shipped data, when a machine has it: NEVR_STOCK_DATA names a stock PC data folder
// (`_data/5932408047/rad15/win10` of an install). Each step's output, per level, must be byte for byte the combat gun
// patch's table that he_is_the_cat's pack starts from; only those files' sha-256 values are kept here, no bytes.
// Skipped without the variable.
var gunPatchTables = map[uint64]map[uint64]string{
	jsonResourceType: {equipmentName: "3ae2cbc89ab64daa9b6989eb23feeb45ce9e0b549aa684ba79f974ad3b42f57d"},
	bulletTableType: {
		0x42670f2bed45703c: "0f8c755be03f720edd229a4d4627facc91a295a6c6336447c3f3c85456aa13c6",
		0x43e2da7914642604: "0f8c755be03f720edd229a4d4627facc91a295a6c6336447c3f3c85456aa13c6",
		0x43e2da7a0c623a19: "0f8c755be03f720edd229a4d4627facc91a295a6c6336447c3f3c85456aa13c6",
		0xcb9977f7fc2b4526: "0f8c755be03f720edd229a4d4627facc91a295a6c6336447c3f3c85456aa13c6",
		0xdf5ca7b7dfa383d4: "0f8c755be03f720edd229a4d4627facc91a295a6c6336447c3f3c85456aa13c6",
	},
	particleTableType: {
		0x42670f2bed45703c: "a207cf39b6665642b2cc39851652e7e6e36fc8fb130a475bda911b617ffd4349",
		0x43e2da7914642604: "a207cf39b6665642b2cc39851652e7e6e36fc8fb130a475bda911b617ffd4349",
		0x43e2da7a0c623a19: "7e3cf1acb3ac7d3696e66e8b4e9ae0d1aa158a723f415cc1188a0aa85b08db2a",
		0xcb9977f7fc2b4526: "a207cf39b6665642b2cc39851652e7e6e36fc8fb130a475bda911b617ffd4349",
		0xdf5ca7b7dfa383d4: "a207cf39b6665642b2cc39851652e7e6e36fc8fb130a475bda911b617ffd4349",
	},
	scriptTable: {
		0x42670f2bed45703c: "d3e95f391a72390fa901cc6ccb2e5c070a992779516de61bcd94dc6797589228",
		0x43e2da7914642604: "0144a28b54260ff82e78fc8a8d8bcbd374ad52f3a91b5557313a4c2b5eec05a0",
		0x43e2da7a0c623a19: "86f09e0a297cf1c2cea39c1c8f2841d0814cb6ed8e2563166ed4e59c0e21b122",
		0xcb9977f7fc2b4526: "8c1cb90d102aa94d9e381c6286c5d64685c45480844798786403d11dba3ef652",
		0xdf5ca7b7dfa383d4: "03c1f3e492286d4a0babe4a34b832f38661b8272cf9188f20742c5847cf72593",
	},
}

// The resources the registered steps change, counted from the step list: the per-level steps each change one table
// per drone level (steps_drone.go: bullets, particle effects, scripts; steps_drone_health.go: the damageable table;
// steps_dot_stick.go: the magazine table; steps_dot_constraints.go: the rigid-attach and parented-constraint tables;
// steps_dot_damp.go: the dampener and transform tables; steps_dot_field.go: the model and transform tables, of which the
// transform table is the one steps_dot_damp.go already changes, so it is one resource, counted once;
// steps_dot_field_texmat.go: the texture-streaming table), steps_equipment.go changes the one mp_equipment resource,
// steps_dot_preset.go the one mp_weapon_settings resource, steps_dot_field_texmat.go adds the two red materials, and
// steps_rules_terminal.go changes the one RULES canvas, in whichever client set lists it, and steps_buff.go changes the
// global level's heal bubble table (its tac-mod step changes mp_equipment, which equipmentResources already counts),
// steps_point.go changes the explosion table, one per drone level (its tac-mod step's mp_equipment is equipmentResources,
// its required_charge step's mp_weapon_settings is weaponSettingsResources: each resource is counted once), and
// steps_veil.go changes only mp_equipment, which equipmentResources counts, so it adds no term, and steps_medigun.go
// changes mp_weapon_settings (weaponSettingsResources) and the global level's heal bubble table, the same resource
// (Sets[0], healBubbleTable, globalLevel) as steps_buff.go's (healBubbleTableResources): each is counted once, so it adds
// no term either, and steps_mapterm.go changes, per map level (mapTermLevels, 4: mpl_lobby_b_combat has no map buttons),
// the level's actor table and each component table that owns a row of the REMATCH subtree; its script and transform
// tables are levelTableSteps' and dotDampTables' resources, counted already. Which component tables those are is the
// shipped data's and is not established without it, so they are counted as they come (mapTermComponentTables), each
// one of actor_copy.py's TYPE_NAMES (mapTermResourcesNamed), and any other resource the step changes fails here.
// steps_buff_fx_arrow.go changes the four records of the OVERCHARGE arrow mesh under fx_overcharge_arrow, one per type,
// all four new resources in Sets[0] (buffArrowResources).
// (5 + 2 + 2 + 2 - 1 + 1 + 1) x 5 + 1 + 1 + 2 + 1 + 1 + 1 x 4 = 70, plus the arrow's 4 and the map buttons' component
// tables, over both client sets.
// The count covers every step, so a step that changes a resource it does not name fails here; what each data step
// writes into its own tables is checked by that step's own test, and the gun patch's files by gunPatchTables above.
const (
	levelTableSteps     = 5
	dotConstraintTables = 2 // steps_dot_constraints.go: the DOT magazine's rigid-attach and parented-constraint rows
	dotDampTables       = 2 // steps_dot_damp.go: the DOT actor's CR15DampenerCR row and CTransformCR rows
	dotFieldTables      = 2 // steps_dot_field.go: the FIELD's CModelCRWin10 records and its CTransformCRWin10 row
	// steps_dot_field.go's transform table is steps_dot_damp.go's CTransformCR table: both steps change it, and a
	// resource is counted once
	dotFieldTablesSharedWithDamp = 1
	// steps_dot_field_texmat.go: the FIELD's CTextureStreamingCRWin10 record, one table per level
	dotFieldTexStreamTables = 1
	// steps_point.go: the explosion table 4af215b19b297c34 that gets the barrier wall's row, one table per level
	pointExplosionTables = 1
	// steps_dot_field_texmat.go: the two red material copies, mat_dot_field_bubble_a and _b, new resources
	dotFieldMaterialResources = 2
	equipmentResources        = 1
	// steps_dot_preset.go: the DOT FIELD's three activation booleans in mp_weapon_settings, which the gun patch's files
	// do not cover; steps_point.go's required_charge and steps_medigun.go's ability_properties.medigun are in the same resource
	weaponSettingsResources = 1
	// steps_rules_terminal.go: the RULES page canvas ui_display_mpl_mm_custom_rules1, which the gun patch's files do not cover
	rulesCanvasResources = 1
	// steps_buff.go: the global level's heal bubble table, one resource; its tac-mod step's mp_equipment is equipmentResources.
	// steps_medigun.go's MEDIGUN bubble rows are in the same table
	healBubbleTableResources = 1
	// steps_buff_fx_arrow.go: the cross's four records under fx_overcharge_arrow, one per type; new resources in Sets[0]
	buffArrowResources = 4
	// steps_mapterm.go: each map level's actor table 347869ce492dc7da, which gets the three copies of the REMATCH subtree
	mapTermActorTables = 1
)

// afterGunPatchCompare are the steps that change mp_equipment past the gun patch's (it has none of OVERCHARGE, NEUTRON and UMBRAL VEIL),
// and the map terminal step, which rewrites the four map levels' script tables past the gun patch's (it has no map buttons),
// so the gun patch's sha-256 values are compared before them.
var afterGunPatchCompare = map[string]bool{buffTacmodStep: true, pointTacmodStep: true, veilStepName: true, mapTermStepName: true}

// mapTermResourcesNamed checks the resources the map terminal step added: each is one of the four map levels' actor table
// (exactly one per level) or one of its component tables that no other term counts (actor_copy.py's TYPE_NAMES without
// CScriptCR, counted by levelTableSteps, and CTransformCR, counted by dotDampTables). It returns how many component
// tables there are, and an error naming each resource no term names.
func mapTermResourcesNamed(added []radpack.Key) (int, error) {
	levels := map[int64]bool{}
	for _, l := range mapTermLevels {
		levels[int64(l.level)] = true
	}
	actors := map[int64]int{}
	components := 0
	var errs []error
	for _, k := range added {
		typ := mtTypeKey(uint64(k.Type))
		_, named := mtTypeNames[typ]
		switch {
		case !levels[k.Name]:
			errs = append(errs, fmt.Errorf("the map terminal changed %016x.%016x, which is not a map level's", uint64(k.Type), uint64(k.Name)))
		case typ == mtActorsType:
			actors[k.Name]++
		case named && uint64(k.Type) != scriptTable && uint64(k.Type) != transformType:
			components++
		default:
			errs = append(errs, fmt.Errorf("the map terminal changed %016x.%016x, a resource no term names", uint64(k.Type), uint64(k.Name)))
		}
	}
	for _, l := range mapTermLevels {
		if n := actors[int64(l.level)]; n != mapTermActorTables {
			errs = append(errs, fmt.Errorf("the map terminal changed level %016x's actor table %d times, want %d", l.level, n, mapTermActorTables))
		}
	}
	return components, errors.Join(errs...)
}

func TestTheEchoCombatStepsReproduceTheGunPatchFilesFromTheShippedData(t *testing.T) {
	data := os.Getenv("NEVR_STOCK_DATA")
	if data == "" {
		t.Skip("NEVR_STOCK_DATA is not set (a stock PC data folder: manifests/ and packages/)")
	}
	p := &Pack{gameDir: filepath.Join(data, "..", "..", "..", ".."), sets: map[string]*radpack.Set{},
		changes: map[string]map[radpack.Key][]byte{}, scripts: map[string][]byte{}}
	for _, setID := range Sets { // both client sets, as Build opens them: a step may change a resource in either
		s, _, err := radpack.OpenSet(data, setID)
		if err != nil {
			t.Fatal(err)
		}
		p.sets[setID] = s
	}
	// The gun patch's mp_equipment has no OVERCHARGE, NEUTRON or UMBRAL VEIL (steps_buff.go, steps_point.go, steps_veil.go
	// add them), and its four map levels' script tables have no map buttons (steps_mapterm.go adds them), so its sha-256
	// values are compared before those steps.
	for _, st := range steps {
		if afterGunPatchCompare[st.Name] {
			continue
		}
		if err := st.Apply(p); err != nil {
			t.Fatalf("%s: %v", st.Name, err)
		}
	}
	for typ, levels := range gunPatchTables {
		for lvl, want := range levels {
			b, ok := p.changes[Sets[0]][radpack.Key{Type: int64(typ), Name: int64(lvl)}]
			if !ok {
				t.Errorf("%016x.%016x was not changed", typ, lvl)
				continue
			}
			sum := sha256.Sum256(b)
			if got := hex.EncodeToString(sum[:]); got != want {
				t.Errorf("%016x.%016x: sha256 %s, the gun patch's is %s", typ, lvl, got, want)
			}
		}
	}
	var mapTermAdded []radpack.Key
	for _, st := range steps {
		if !afterGunPatchCompare[st.Name] {
			continue
		}
		before := map[radpack.Key]bool{}
		for k := range p.changes[Sets[0]] {
			before[k] = true
		}
		if err := st.Apply(p); err != nil {
			t.Fatalf("%s: %v", st.Name, err)
		}
		if st.Name != mapTermStepName {
			continue
		}
		for k := range p.changes[Sets[0]] {
			if !before[k] {
				mapTermAdded = append(mapTermAdded, k)
			}
		}
	}
	mapTermComponentTables, err := mapTermResourcesNamed(mapTermAdded)
	if err != nil {
		t.Error(err)
	}
	t.Logf("the map terminal changed %d component tables over %d levels", mapTermComponentTables, len(mapTermLevels))
	// The RULES canvas (steps_rules_terminal.go) is not a gun patch file, and the step changes it in whichever client set
	// lists it: it is counted as its own term, and taken out of that set's changes so the per-set count below is the
	// other steps' resources.
	var canvasType, canvasName uint64 = rulesCanvasType, rulesCanvasName
	canvas := radpack.Key{Type: int64(canvasType), Name: int64(canvasName)}
	held := 0
	for _, setID := range Sets {
		if _, ok := p.changes[setID][canvas]; ok {
			delete(p.changes[setID], canvas)
			held++
		}
	}
	if held != rulesCanvasResources {
		t.Errorf("the RULES canvas was changed in %d client sets, want %d", held, rulesCanvasResources)
	}
	changed := held
	for _, setID := range Sets {
		changed += len(p.changes[setID])
	}
	levelTables := levelTableSteps + dotConstraintTables + dotDampTables + dotFieldTables - dotFieldTablesSharedWithDamp + dotFieldTexStreamTables + pointExplosionTables
	if want := levelTables*len(droneLevels) + equipmentResources + weaponSettingsResources + dotFieldMaterialResources + rulesCanvasResources + healBubbleTableResources +
		buffArrowResources + mapTermActorTables*len(mapTermLevels) + mapTermComponentTables; changed != want {
		t.Errorf("%d resources changed, want %d (%d level tables + %d DOT constraint tables + %d DOT dampener tables + %d DOT FIELD tables - %d shared + %d DOT FIELD texture streaming table + %d explosion table, x %d levels, mp_equipment, mp_weapon_settings, %d red materials, the RULES canvas, the global level's heal bubble table, %d OVERCHARGE arrow records, %d actor table x %d map levels and %d map-button component tables)", changed,
			want, levelTableSteps, dotConstraintTables, dotDampTables, dotFieldTables, dotFieldTablesSharedWithDamp, dotFieldTexStreamTables, pointExplosionTables, len(droneLevels), dotFieldMaterialResources,
			buffArrowResources, mapTermActorTables, len(mapTermLevels), mapTermComponentTables)
	}
	t.Log(fmt.Sprintf("%d resources match the gun patch's", len(p.changes[Sets[0]])))
}
