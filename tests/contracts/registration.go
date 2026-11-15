package contracts

import (
	"errors"
	"net/url"
	"sort"
	"strconv"
	"strings"
)

// #1536: a game server registers with identity plus measured facts only. Which guilds and regions it serves is the game
// service's decision, returned in the answer (Assignment). `guilds` and `regions` stay as optional overrides while a
// config.yaml still sets them (src/runtime/server/serverdb_uri.h today), and are never sent otherwise.
type RegistrationFacts struct {
	Version  string
	Address  string
	PingMs   map[string]int // region -> measured ping
	Features []string
}

type Overrides struct{ Guilds, Regions []string }

// RegistrationQuery is the query string appended to the ServerDB registration URI.
func RegistrationQuery(f RegistrationFacts, o Overrides) string {
	v := url.Values{}
	if f.Version != "" {
		v.Set("version", f.Version)
	}
	if f.Address != "" {
		v.Set("address", f.Address)
	}
	if len(f.PingMs) > 0 {
		regs := make([]string, 0, len(f.PingMs))
		for r := range f.PingMs {
			regs = append(regs, r)
		}
		sort.Strings(regs)
		parts := make([]string, 0, len(regs))
		for _, r := range regs {
			parts = append(parts, r+":"+strconv.Itoa(f.PingMs[r]))
		}
		v.Set("ping_ms", strings.Join(parts, ","))
	}
	if len(f.Features) > 0 {
		v.Set("features", strings.Join(f.Features, ","))
	}
	if len(o.Guilds) > 0 {
		v.Set("guilds", strings.Join(o.Guilds, ","))
	}
	if len(o.Regions) > 0 {
		v.Set("regions", strings.Join(o.Regions, ","))
	}
	return v.Encode()
}

// Assignment is the service's answer: the guilds (uuids) and regions this server serves. Both lists may be empty: the
// server stays registered and idle.
type Assignment struct {
	Guilds  []string `json:"guilds"`
	Regions []string `json:"regions"`
}

func ParseAssignment(b []byte) (Assignment, error) {
	var a Assignment
	if err := strictDecode(b, &a); err != nil {
		return a, err
	}
	if a.Guilds == nil || a.Regions == nil {
		return a, errors.New("guilds and regions are both required (empty lists allowed)")
	}
	for _, g := range a.Guilds {
		if !uuidRe.MatchString(g) {
			return a, errors.New("guild id is not a lower-case uuid")
		}
	}
	return a, nil
}
