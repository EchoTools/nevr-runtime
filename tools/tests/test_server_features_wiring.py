"""#2333: the server states EchoCombat and MEDIGUN; clients read that, not VeilNet's hello.

Source sensors for what the GTests cannot reach. test_server_features.cpp drives the statement and the gate
(compat/server_features.h), test_lobby_settings_relay.cpp the server's document, test_registration_roundtrip.cpp
the registration's `features`, and test_lobby_settings_hook.cpp the client's hook. What no test target links:

  - VeilNet (combat/veilnet.cpp) decides the three tac mods and MEDIGUN from nevr_server_features::Client(),
    and neither sends nor reads an 'A' answer to the hello;
  - the server raises its document once a session has started (gameserver_callbacks.cpp), and the relay sets its
    key in the service's document;
  - the registration's features come from the combat module (registration_metadata.cpp);
  - a client clears what the last server stated when it gets a session in another lobby (ws_bridge.cpp);
  - the hook is installed (initialize.cpp).
"""

import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function, strip_comments


ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text()


def code(path: str) -> str:
    return strip_comments(read(path))


class VeilNetReadsTheServersStatement(unittest.TestCase):
    def setUp(self):
        self.src = code("src/runtime/combat/veilnet.cpp")

    def test_the_gear_lists_follow_the_statement(self):
        body = extract_braced_function(self.src, "void UpdateGearLists()")
        self.assertIn("nevr_server_features::Client().Current().echoCombat", body)
        self.assertIn("TacModsFor(answered)", body)

    def test_medigun_follows_the_statement(self):
        definition = re.search(r"bool MedigunAllowedNow\(\) \{([^}]*)\}", self.src)
        self.assertIsNotNone(definition, "MedigunAllowedNow is defined")
        self.assertIn("nevr_server_features::Client().Current().medigun", definition.group(1))

    def test_no_answer_is_sent_or_read(self):
        self.assertNotRegex(self.src, r"Send\(\s*s\s*,\s*from\s*,\s*'A'")
        self.assertNotRegex(self.src, r"p\.type\s*==\s*'A'")
        for gone in ("g_last_answer", "SERVER_TIMEOUT", "ANSWER_MEDIGUN"):
            self.assertNotIn(gone, self.src, gone)

    def test_the_hello_stays_the_relay_subscription(self):
        self.assertIn("SendToServer('H', 0)", self.src)
        self.assertRegex(self.src, r"if \(p\.type == 'H'\) \{\s*uint32_t total")


class TheServerStatesItsFeatures(unittest.TestCase):
    def test_a_new_session_states_them_after_it_started(self):
        src = code("src/runtime/server/gameserver_callbacks.cpp")
        start = src.index("case gameservice::v1::Envelope::kLobbySessionCreate:")
        end = src.index("case gameservice::v1::Envelope::kLobbySessionEvent:")
        case = src[start:end]
        self.assertIn("StateServerFeatures(self);", case)
        self.assertLess(case.index("StartSession()"), case.index("StateServerFeatures(self);"),
                        "the session start clears the last document's record first")

    def test_the_relay_sets_the_servers_key(self):
        src = code("src/runtime/server/gameserver_callbacks.cpp")
        body = extract_braced_function(src, "static void RelayLobbySettingsFrame(GameServerLib* self, const VOID* data, UINT64 size)")
        self.assertIn("nevr_combat::ServerFeatures()", body)
        self.assertRegex(body, r"RelayLobbySettings\(data, size, session, sinks, &features\)")
        self.assertIn("record.json = report.serviceJson;", body)

    def test_the_registration_carries_the_combat_modules_features(self):
        src = code("src/runtime/server/registration_metadata.cpp")
        self.assertIn("nevr_combat::ServerFeatures()", src)
        self.assertIn("params.features = std::move(features);", src)
        self.assertIn("registration->add_features(feature)", code("src/runtime/server/registration_envelope.cpp"))

    def test_the_combat_module_computes_them_from_what_started(self):
        src = code("src/runtime/combat/combat.cpp")
        self.assertRegex(src, r"CombatFeatures\(veil, at\(Group::kMedigun\)\.on, overcharge\)")


class TheClientReadsAndClears(unittest.TestCase):
    def test_the_hook_is_installed(self):
        self.assertIn("nevr_lobby_settings_hook::Install(", code("src/runtime/lifecycle/initialize.cpp"))

    def test_a_session_in_another_lobby_clears_the_statement(self):
        src = code("src/runtime/compat/ws_bridge.cpp")
        success = re.search(r"case nevr_lobby_frame::Kind::Success:(.*?)break;", src, re.S)
        self.assertIsNotNone(success)
        self.assertIn("ClearServerFeaturesOnNewLobby(state.lobby);", success.group(1))
        self.assertIn("nevr_server_features::Client().Clear()", src)


if __name__ == "__main__":
    unittest.main()
