"""Source contracts for the bridge's owned, ordered callback boundary."""

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
    def test_local_callback_admits_owned_events_and_orders_open_route(self):
        bridge = source("src/runtime/compat/ws_bridge.cpp")
        callback = section(bridge, "auto onClientMessage = GuardWsCallback(",
                           "if (!g_bridgeOwner.Start())")
        self.assertIn("std::unique_lock<std::mutex> admissionLock(g_bridgeAdmissionMutex)", callback)
        self.assertIn("RegisterConnection(&gameWs)", callback)
        self.assertIn("FindConnection(&gameWs)", callback)
        self.assertIn("CloseAndPost(", callback)
        self.assertIn("PostTerminalCloseWithSequence(", callback)
        self.assertIn("PostIfAccepting([&]", callback)
        self.assertIn("PostDataWithSequence", callback)
        self.assertIn("PostControlWithSequence", callback)
        open_publish = callback.index("SetActiveConnection(connection)")
        open_enqueue = callback.index("PostControlWithSequence", open_publish)
        self.assertLess(open_publish, open_enqueue)
        self.assertIn("connection->lifetime->IsRetired()", callback)
        self.assertIn("processClientMessage(connState, gameWs, queuedMessage, sequence)", callback)
        self.assertNotIn("TryAcquireClose", callback)
        self.assertNotIn("PostFence", callback)

    def test_shared_remote_snapshots_and_enqueues_under_same_admission(self):
        bridge = source("src/runtime/compat/ws_bridge.cpp")
        remote = section(bridge, "static void PostRemoteEvent(",
                         "// The login connection's remote WS")
        self.assertIn("std::unique_lock<std::mutex> admissionLock(g_bridgeAdmissionMutex)", remote)
        route_snapshot = remote.index("GetActiveConnection()")
        target_lease = remote.index("target->lifetime->TryAcquire()", route_snapshot)
        queue_append = remote.index("PostDataWithSequence", target_lease)
        self.assertLess(route_snapshot, target_lease)
        self.assertLess(target_lease, queue_append)
        self.assertIn("PostIfBothAccepting", remote)
        self.assertIn("sourceLease", remote)
        self.assertIn("targetLease", remote)
        self.assertIn("target->lifetime->IsRetired()", remote)
        self.assertIn("handler(pair, target, ownedMessage, sequence, receiveEvent)", remote)
        self.assertNotIn("TryAcquire()", remote[queue_append:])

    def test_close_is_fifo_reserved_and_never_uses_priority_fence(self):
        bridge = source("src/runtime/compat/ws_bridge.cpp")
        close_helper = section(bridge, "static void ScheduleConnectionClose(",
                              "static std::shared_ptr<BridgeConnectionContext> FindConnection(")
        self.assertIn("CloseAndPost(", close_helper)
        self.assertIn("PostTerminalCloseWithSequence(", close_helper)
        self.assertIn("WaitForLeases()", close_helper)
        self.assertIn("CleanupConnectionOnOwner(connection)", close_helper)
        self.assertIn("Bridge queue limit reached; closing connection", close_helper)
        self.assertNotIn("PostFence", bridge)

    def test_shutdown_serializes_close_admission_then_drains_before_join(self):
        bridge = source("src/runtime/compat/ws_bridge.cpp")
        stop = section(bridge, "void StopWebSocketBridgeListener() {", "// N61 behavioral test hooks")
        admission_lock = stop.index("g_bridgeAdmissionMutex")
        connections_lock = stop.index("g_connectionsMutex", admission_lock)
        close_admission = stop.index("connection->lifetime->CloseAdmission()")
        self.assertLess(admission_lock, connections_lock)
        self.assertLess(connections_lock, close_admission)
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

    def test_behavioral_target_links_owner_implementation(self):
        cmake = source("src/runtime/CMakeLists.txt")
        behavioral = section(cmake, "add_executable(test_behavioral", "target_compile_definitions(test_behavioral")
        self.assertIn("compat/bridge_control_owner.cpp", behavioral)


if __name__ == "__main__":
    unittest.main()
