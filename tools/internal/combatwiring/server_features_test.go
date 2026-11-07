package combatwiring

import (
	"regexp"
	"strings"
	"testing"
)

// #2333: the server states EchoCombat and MEDIGUN; clients read that, not VeilNet's hello. test_server_features.cpp
// drives the statement and the gate (compat/server_features.h), test_lobby_settings_relay.cpp the server's document,
// test_registration_roundtrip.cpp the registration's features and test_lobby_settings_hook.cpp the client's hook. These
// pin what no test target links: VeilNet's gates read the statement and no 'A' is sent or read; the server raises its
// document once a session has started and the relay sets its key; the registration takes the combat module's features;
// a session in another lobby clears the statement; the hook is installed.

func TestTheGearListsFollowTheServersStatement(t *testing.T) {
	gear := body(t, code(t, veilnet), "void UpdateGearLists()")
	if !strings.Contains(gear, "nevr_server_features::Client().Current().echoCombat") || !strings.Contains(gear, "TacModsFor(answered)") {
		t.Errorf("UpdateGearLists no longer decides the tac mods from the server's statement:\n%s", gear)
	}
}

func TestMedigunFollowsTheServersStatement(t *testing.T) {
	m := regexp.MustCompile(`bool MedigunAllowedNow\(\) \{([^}]*)\}`).FindStringSubmatch(code(t, veilnet))
	if m == nil || !strings.Contains(m[1], "nevr_server_features::Client().Current().medigun") {
		t.Errorf("MedigunAllowedNow no longer reads the server's statement: %q", m)
	}
}

func TestTheAnswerIsSentForOlderClientsAndReadByNone(t *testing.T) {
	src := code(t, veilnet)
	// The Quest combat APK players have (r15 61700999) shows the tac mods only after an 'A': the server keeps answering.
	if !regexp.MustCompile(`if \(p\.type == 'H'\) \{\s*Send\(s, from, 'A', g_medigun_on_server \? kAnswerMedigun : 0\);`).MatchString(src) {
		t.Error("the server no longer answers a hello with an 'A' (clients of older builds need it)")
	}
	if regexp.MustCompile(`p\.type\s*==\s*'A'`).MatchString(src) {
		t.Error("a client reads an 'A' again")
	}
	for _, gone := range []string{"g_last_answer", "SERVER_TIMEOUT", "g_last_answer_flags"} {
		if strings.Contains(src, gone) {
			t.Errorf("%s is back", gone)
		}
	}
}

func TestTheHelloStaysTheRelaySubscription(t *testing.T) {
	src := code(t, veilnet)
	if !strings.Contains(src, "SendToServer('H', 0)") {
		t.Error("the client no longer says hello: the server would relay to nobody")
	}
	if !regexp.MustCompile(`'A', g_medigun_on_server \? kAnswerMedigun : 0\);\s*uint32_t total`).MatchString(src) {
		t.Error("the server's hello branch no longer sends the RULES and LOADOUTS words after the answer")
	}
}

const callbacks = "src/runtime/server/gameserver_callbacks.cpp"

func TestANewSessionStatesTheFeaturesAfterItStarted(t *testing.T) {
	src := code(t, callbacks)
	start := strings.Index(src, "case gameservice::v1::Envelope::kLobbySessionCreate:")
	end := strings.Index(src, "case gameservice::v1::Envelope::kLobbySessionEvent:")
	if start < 0 || end < start {
		t.Fatal("no session create case")
	}
	c := src[start:end]
	st, call := strings.Index(c, "StartSession()"), strings.Index(c, "StateServerFeatures(self);")
	if call < 0 || st < 0 || st > call {
		t.Errorf("the session create case does not state the features after StartSession (which clears the last record):\n%s", c)
	}
}

func TestTheRelaySetsTheServersKey(t *testing.T) {
	relay := body(t, code(t, callbacks), "static void RelayLobbySettingsFrame(GameServerLib* self, const VOID* data, UINT64 size)")
	for _, want := range []string{"nevr_combat::ServerFeatures()", "RelayLobbySettings(data, size, session, sinks, &features)",
		"record.json = report.serviceJson;"} {
		if !strings.Contains(relay, want) {
			t.Errorf("the relay lost %q:\n%s", want, relay)
		}
	}
}

func TestTheRegistrationCarriesTheCombatModulesFeatures(t *testing.T) {
	meta := code(t, "src/runtime/server/registration_metadata.cpp")
	if !strings.Contains(meta, "nevr_combat::ServerFeatures()") || !strings.Contains(meta, "params.features = std::move(features);") {
		t.Error("the registration metadata no longer takes the combat module's features")
	}
	if !strings.Contains(code(t, "src/runtime/server/registration_envelope.cpp"), "registration->add_features(feature)") {
		t.Error("the envelope no longer writes field 13")
	}
}

func TestTheCombatModuleComputesThemFromWhatStarted(t *testing.T) {
	if !regexp.MustCompile(`CombatFeatures\(veil, at\(Group::kMedigun\)\.on, overcharge\)`).MatchString(code(t, "src/runtime/combat/combat.cpp")) {
		t.Error("combat.cpp no longer computes the features from what started")
	}
}

func TestTheClientsHookIsInstalled(t *testing.T) {
	if !strings.Contains(code(t, "src/runtime/lifecycle/initialize.cpp"), "nevr_lobby_settings_hook::Install(") {
		t.Error("initialize.cpp no longer installs the lobby settings hook")
	}
}

func TestASessionInAnotherLobbyClearsTheStatement(t *testing.T) {
	src := code(t, "src/runtime/compat/ws_bridge.cpp")
	m := regexp.MustCompile(`(?s)case nevr_lobby_frame::Kind::Success:(.*?)break;`).FindStringSubmatch(src)
	if m == nil || !strings.Contains(m[1], "ClearServerFeaturesOnNewLobby(state.lobby);") {
		t.Errorf("a lobby session success no longer clears the last server's statement: %q", m)
	}
	if !strings.Contains(src, "nevr_server_features::Client().ClearForLobby(lobby, &wasOpen)") {
		t.Error("ClearServerFeaturesOnNewLobby no longer clears the client state for a new lobby")
	}
	router := code(t, "src/runtime/compat/session_router.cpp")
	if !strings.Contains(router, "nevr_server_features::Client().ClearForLobby(success.lobby, &wasOpen)") {
		t.Error("the session router (Quest) no longer clears the client state on a success in another lobby")
	}
}
