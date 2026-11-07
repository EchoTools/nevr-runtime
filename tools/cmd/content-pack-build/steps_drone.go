package main

import (
	"fmt"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
)

// The DRONE's bullet, particle and script rows (#600, EchoCombat slice 5). he_is_the_cat's pack starts from a
// community "combat gun patch" whose level tables differ from the stock ones in exactly these three rules, measured
// against the shipped PC data (the gun patch's file of each, per level, is reproduced byte for byte: stock_steps_test.go
// pins its sha-256). Each is a rule over the player's own stock rows; no row of the game's is in this repository.
//
//  1. Bullets (CR15NetBulletCR on disk, 4fd65c8d2b63d470): rows are keyed by bullet id at +8. The drone bullet's row
//     takes the assault bullet's two particle-effect ids at +0x58 and +0x60, so a drone shot shows the assault's effects.
//  2. Particle effects (21a09c2016d8f3e6): rows are keyed by bullet id at +8. Each row of the assault bullet is copied
//     to the end of the table with +8 set to the drone bullet.
//  3. Scripts (d99f6bbd8009c92c): rows name a script at +0 and an actor at +8. Before the first row of the drone actor,
//     a copy of the first row of script 190ed14fe6c74dd3 is inserted with +8 set to the drone actor, so the drone's
//     actor runs that script too.
const (
	bulletTableType   = 0x4fd65c8d2b63d470
	particleTableType = 0x21a09c2016d8f3e6
	scriptTable       = 0xd99f6bbd8009c92c
	assaultBullet     = 0x12001003b8e9a2a6
	droneBullet       = 0xa8f84e7dd40b806e
	droneActor        = 0x10e40287a9bf5a13
	sharedScript      = 0x190ed14fe6c74dd3
	keyOff            = 0x08 // the bullet (or actor) a row is for
	effectsOff        = 0x58 // a bullet row's two particle-effect ids, +0x58 and +0x60
	effectsLen        = 0x10
)

// droneLevels are the five combat levels whose tables carry the drone: mpl_combat_combustion, _dyson, _gauss, _fission
// and mpl_lobby_b_combat (#1420 names the same five).
var droneLevels = []uint64{0x42670f2bed45703c, 0x43e2da7914642604, 0x43e2da7a0c623a19, 0xcb9977f7fc2b4526, 0xdf5ca7b7dfa383d4}

func init() {
	steps = append(steps,
		Step{Name: "drone bullet takes the assault bullet's effects", Apply: forEachLevel(bulletTableType, droneBulletEffects)},
		Step{Name: "drone bullet gets the assault bullet's particle rows", Apply: forEachLevel(particleTableType, droneParticleRows)},
		Step{Name: "drone actor runs script 190ed14fe6c74dd3", Apply: forEachLevel(scriptTable, droneScriptRow)},
	)
}

func forEachLevel(typ uint64, rule func(*radpack.RowTable) error) func(*Pack) error {
	return func(p *Pack) error {
		for _, lvl := range droneLevels {
			b, err := p.Read(Sets[0], int64(typ), int64(lvl))
			if err != nil {
				return err
			}
			t, err := radpack.ParseRowTable(b)
			if err != nil {
				return fmt.Errorf("level %016x: %w", lvl, err)
			}
			if err := rule(t); err != nil {
				return fmt.Errorf("level %016x: %w", lvl, err)
			}
			if err := p.Put(Sets[0], int64(typ), int64(lvl), t.Bytes()); err != nil {
				return err
			}
		}
		return nil
	}
}

func droneBulletEffects(t *radpack.RowTable) error {
	donor, drone := t.Find(keyOff, assaultBullet), t.Find(keyOff, droneBullet)
	if donor < 0 || drone < 0 || t.Size < effectsOff+effectsLen {
		return fmt.Errorf("no assault (%d) or drone (%d) bullet row of %d bytes", donor, drone, t.Size)
	}
	copy(t.Rows[drone][effectsOff:effectsOff+effectsLen], t.Rows[donor][effectsOff:effectsOff+effectsLen])
	return nil
}

func droneParticleRows(t *radpack.RowTable) error {
	if t.Find(keyOff, droneBullet) >= 0 {
		return fmt.Errorf("the table already has a drone bullet row")
	}
	n := 0
	for _, r := range t.Rows[:len(t.Rows):len(t.Rows)] {
		if radpack.U64(r, keyOff) != assaultBullet {
			continue
		}
		c := append([]byte(nil), r...)
		radpack.SetU64(c, keyOff, droneBullet)
		t.Rows = append(t.Rows, c)
		n++
	}
	if n == 0 {
		return fmt.Errorf("no assault bullet row to copy")
	}
	return nil
}

func droneScriptRow(t *radpack.RowTable) error {
	at, src := t.Find(keyOff, droneActor), t.Find(0, sharedScript)
	if at < 0 || src < 0 {
		return fmt.Errorf("no drone actor row (%d) or script 190ed14fe6c74dd3 row (%d)", at, src)
	}
	c := append([]byte(nil), t.Rows[src]...)
	radpack.SetU64(c, keyOff, droneActor)
	t.Rows = append(t.Rows[:at], append([][]byte{c}, t.Rows[at:]...)...)
	return nil
}
