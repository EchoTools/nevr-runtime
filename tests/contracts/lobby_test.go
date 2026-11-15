package contracts

import "testing"

func TestEveryLobbyQueuesForEveryMode(t *testing.T) {
	for _, k := range []string{"combat", "arena", "combat_mature", "arena_mature", "a_kind_added_later"} {
		for _, m := range []string{"arena", "combat", "ai", "private"} {
			if !CanQueue(ParseLobbyKind(k), m) {
				t.Errorf("lobby %q cannot queue for %q", k, m)
			}
		}
	}
}

func TestDefaultLobbyIsCombat(t *testing.T) {
	if DefaultLobbyKind != LobbyCombat || ParseLobbyKind("") != LobbyCombat {
		t.Fatal("the default lobby is combat")
	}
	if ParseLobbyKind("a_kind_added_later") != LobbyCombat {
		t.Fatal("a kind this client does not know falls back to the default")
	}
}

func TestQuestArenaLobbyLoadsNoCombatSection(t *testing.T) {
	if LoadsCombatSection(LobbyArena, "quest") || LoadsCombatSection(LobbyArenaMature, "quest") {
		t.Error("an arena lobby on Quest must not load the combat section")
	}
	if !LoadsCombatSection(LobbyCombat, "quest") || !LoadsCombatSection(LobbyCombatMature, "quest") {
		t.Error("a combat lobby loads the combat section")
	}
	if !LoadsCombatSection(LobbyArena, "pc") {
		t.Error("PC is not changed by this contract")
	}
}

func TestMatureVariants(t *testing.T) {
	if !LobbyArenaMature.Mature() || LobbyArena.Mature() || !LobbyCombatMature.Mature() {
		t.Fatal("mature flag")
	}
}
