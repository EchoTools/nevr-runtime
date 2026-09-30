"""Source contract for the bridge's owner-thread callback boundary."""

from pathlib import Path
import unittest


REPO = Path(__file__).resolve().parents[2]


def source(path: str) -> str:
    return (REPO / path).read_text(encoding="utf-8")


def section(text: str, start: str, end: str) -> str:
    start_at = text.index(start)
    end_at = text.index(end, start_at + len(start))
    return text[start_at:end_at]


class BridgeCallbackOwnershipTest(unittest.TestCase):
    def test_local_callback_registers_context_and_closes_with_a_fence_and_lease_drain(self):
        bridge = source("src/runtime/compat/ws_bridge.cpp")
        callback = section(bridge, "auto onClientMessage = GuardWsCallback(",
                           "if (!g_bridgeOwner.Start())")
        self.assertIn("RegisterConnection(&gameWs)", callback)
        self.assertIn("FindConnection(&gameWs)", callback)
        self.assertIn("connection->lifetime->TryAcquire()", callback)
        self.assertIn("connection->lifetime->TryAcquireClose()", callback)
        self.assertIn("g_bridgeOwner.InvokeControlAndWait", callback)
        self.assertIn("connection->lifetime->WaitForLeases()", callback)
        self.assertIn("g_bridgeOwner.PostData", callback)
        self.assertIn("g_bridgeOwner.PostControl", callback)
        task = section(callback, "auto task = [processClientMessage", "const auto posted =")
        self.assertLess(task.index("if (!connection->lifetime->IsOpen()) return;"),
                        task.index("processClientMessage(connState, gameWs, msg);"))
        processor = section(bridge, "auto processClientMessage = GuardWsCallback(",
                            "auto onClientMessage = GuardWsCallback(")
        self.assertIn("const int connIdx = g_connectionCount.fetch_add", processor)

    def test_all_size_and_queue_rejections_close_through_reserved_fence(self):
        bridge = source("src/runtime/compat/ws_bridge.cpp")
        close_helper = section(bridge, "static void ScheduleConnectionClose(",
                              "static std::shared_ptr<BridgeConnectionContext> FindConnection(")
        self.assertIn("connection->lifetime->TryAcquire()", close_helper)
        self.assertIn("connection->lifetime->RequestClose()", close_helper)
        self.assertNotIn("connection->lifetime->CloseAdmission()", close_helper)
        self.assertIn("g_bridgeOwner.PostFence", close_helper)
        self.assertIn("connection->gameWs->close(1009, \"bridge queue limit\")", close_helper)
        self.assertIn("Bridge queue limit reached; closing connection", close_helper)
        self.assertGreaterEqual(bridge.count("ScheduleConnectionClose(connection);"), 5)
        close_callback = section(bridge, "if (msg->type == ix::WebSocketMessageType::Close) {",
                                "auto lease = connection->lifetime->TryAcquire();")
        self.assertIn("connection->lifetime->TryAcquireClose()", close_callback)
        self.assertIn("processClientMessage(connState, gameWs, msg);", close_callback)
        self.assertIn("kMaxPendingMessages", bridge)
        self.assertIn("kMaxPendingBytes", bridge)

    def test_remote_callbacks_use_weak_pairs_owned_messages_and_async_dispatch(self):
        bridge = source("src/runtime/compat/ws_bridge.cpp")
        self.assertGreaterEqual(bridge.count("std::weak_ptr<ProxyPair> weakPair = pair;"), 2)
        self.assertGreaterEqual(bridge.count("connection->lifetime->TryAcquire()"), 4)
        self.assertGreaterEqual(bridge.count("std::make_shared<ix::WebSocketMessage>(*rmsg)"), 2)
        self.assertIn("g_bridgeOwner.PostData(rmsg->str.size(), std::move(handler))", bridge)
        self.assertIn("g_bridgeOwner.PostControl(std::move(handler))", bridge)
        self.assertNotIn("auto* pairPtr = pair.get()", bridge)

    def test_shutdown_stops_listeners_before_fencing_leases_and_joining_owner(self):
        bridge = source("src/runtime/compat/ws_bridge.cpp")
        stop = section(bridge, "void StopWebSocketBridgeListener() {", "// N61 behavioral test hooks")
        ordered = [
            "connection->lifetime->CloseAdmission()",
            "g_server->stop()",
            "connection->lifetime->WaitForLeases()",
            "g_bridgeOwner.InvokeControlAndWait",
            "ws->stop()",
            "g_bridgeOwner.StopAndJoin()",
        ]
        positions = [stop.index(item) for item in ordered]
        self.assertEqual(positions, sorted(positions))

    def test_behavioral_bridge_target_links_owner_implementation(self):
        cmake = source("src/runtime/CMakeLists.txt")
        behavioral = section(cmake, "add_executable(test_behavioral", "target_compile_definitions(test_behavioral")
        self.assertIn("compat/bridge_control_owner.cpp", behavioral)


if __name__ == "__main__":
    unittest.main()
