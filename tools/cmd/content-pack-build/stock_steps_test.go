package main

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"os"
	"path/filepath"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
)

// The DRONE rows against the shipped data, when a machine has it: NEVR_STOCK_DATA names a stock PC data folder
// (`_data/5932408047/rad15/win10` of an install). Each step's output, per level, must be byte for byte the combat gun
// patch's table that he_is_the_cat's pack starts from; only those files' sha-256 values are kept here, no bytes.
// Skipped without the variable.
var gunPatchTables = map[uint64]map[uint64]string{
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

func TestTheDroneStepsReproduceTheGunPatchTablesFromTheShippedData(t *testing.T) {
	data := os.Getenv("NEVR_STOCK_DATA")
	if data == "" {
		t.Skip("NEVR_STOCK_DATA is not set (a stock PC data folder: manifests/ and packages/)")
	}
	s, _, err := radpack.OpenSet(data, Sets[0])
	if err != nil {
		t.Fatal(err)
	}
	p := &Pack{gameDir: filepath.Join(data, "..", "..", "..", ".."), sets: map[string]*radpack.Set{Sets[0]: s},
		changes: map[string]map[radpack.Key][]byte{}, scripts: map[string][]byte{}}
	for _, st := range steps {
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
	if n := len(p.changes[Sets[0]]); n != 15 {
		t.Errorf("%d resources changed, want 15 (3 tables x 5 levels)", n)
	}
	t.Log(fmt.Sprintf("%d tables match the gun patch's", len(p.changes[Sets[0]])))
}
