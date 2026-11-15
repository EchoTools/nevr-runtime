package contracts

import (
	"errors"
	"strings"
)

// #2356: nakama chooses each client's cargo manifest. The login settings carry `nevr_cargo`; the client sends its
// `nevr_install` (a random per-install uuid) in the login profile. The client checks every hash.
//
//	nevr_cargo = {schema:1, session?:{url,sha256}, guilds?:{<group id>:{url,sha256}}}
//
// Precedence, most specific first: the row for the lobby's guild, the session's choice, the client's default. A manifest
// mounts only when its sha256 is one nakama named or the default's.
type CargoChoice struct {
	URL    string `json:"url"`
	SHA256 string `json:"sha256"`
}

type CargoLogin struct {
	Install  string
	Session  *CargoChoice
	PerGuild map[string]CargoChoice
}

type cargoWire struct {
	Install string `json:"nevr_install,omitempty"`
	Cargo   *struct {
		Schema  int                    `json:"schema"`
		Session *CargoChoice           `json:"session,omitempty"`
		Guilds  map[string]CargoChoice `json:"guilds,omitempty"`
	} `json:"nevr_cargo,omitempty"`
}

func (c CargoChoice) valid() bool {
	return strings.HasPrefix(c.URL, "https://") && sha256Re.MatchString(c.SHA256)
}

func ParseCargoLogin(b []byte) (CargoLogin, error) {
	var w cargoWire
	var l CargoLogin
	if err := strictDecode(b, &w); err != nil {
		return l, err
	}
	if w.Install != "" && !uuidRe.MatchString(w.Install) {
		return l, errors.New("nevr_install is not a lower-case uuid")
	}
	l.Install = w.Install
	if w.Cargo == nil {
		return l, nil
	}
	if w.Cargo.Schema != 1 {
		return l, errors.New("nevr_cargo schema must be 1")
	}
	if w.Cargo.Session != nil {
		if !w.Cargo.Session.valid() {
			return l, errors.New("session manifest needs an https url and a lower-case sha256")
		}
		l.Session = w.Cargo.Session
	}
	for g, c := range w.Cargo.Guilds {
		if !uuidRe.MatchString(g) || !c.valid() {
			return l, errors.New("guild row needs a uuid key, an https url and a lower-case sha256")
		}
	}
	l.PerGuild = w.Cargo.Guilds
	return l, nil
}

func SelectManifest(l CargoLogin, lobbyGroupID string, def CargoChoice) CargoChoice {
	if c, ok := l.PerGuild[lobbyGroupID]; ok {
		return c
	}
	if l.Session != nil {
		return *l.Session
	}
	return def
}

// MountAllowed is the falsifier's rule: a manifest whose hash is neither one nakama named nor the default's never mounts.
func MountAllowed(sha string, l CargoLogin, def CargoChoice) bool {
	if sha == def.SHA256 || (l.Session != nil && sha == l.Session.SHA256) {
		return true
	}
	for _, c := range l.PerGuild {
		if sha == c.SHA256 {
			return true
		}
	}
	return false
}
