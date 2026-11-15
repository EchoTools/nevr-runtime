package contracts

// #1859: lobby kinds. The service names the lobby a player is in by kind; the lobby never limits what the player can queue
// for. A kind this client does not know falls back to the default (combat), so a new kind needs no client release to work.
type LobbyKind string

const (
	LobbyCombat       LobbyKind = "combat"
	LobbyArena        LobbyKind = "arena"
	LobbyCombatMature LobbyKind = "combat_mature"
	LobbyArenaMature  LobbyKind = "arena_mature"

	DefaultLobbyKind = LobbyCombat
)

func ParseLobbyKind(s string) LobbyKind {
	switch k := LobbyKind(s); k {
	case LobbyCombat, LobbyArena, LobbyCombatMature, LobbyArenaMature:
		return k
	}
	return DefaultLobbyKind
}

func (k LobbyKind) Mature() bool { return k == LobbyCombatMature || k == LobbyArenaMature }

func (k LobbyKind) arena() bool { return k == LobbyArena || k == LobbyArenaMature }

// CanQueue: every lobby can queue for every mode, at any time. It exists so a test fails the day a lobby kind starts to
// limit a mode.
func CanQueue(LobbyKind, string) bool { return true }

// LoadsCombatSection: on Quest an arena lobby does not load the combat section (the combat lobby takes about 3x as long
// to load and costs battery for a section an arena-only player never visits). PC is unchanged.
func LoadsCombatSection(k LobbyKind, platform string) bool {
	return !(platform == "quest" && k.arena())
}
