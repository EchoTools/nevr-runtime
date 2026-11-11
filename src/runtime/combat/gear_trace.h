#pragma once
// [NEVR.COMBAT] gear ... records (measure only): whether a server's pushed balance settings outlive its session on a PC
// client. The addresses, the values read and the line text are in gear_trace_rules.h.

#include <cstdint>

namespace nevr_gear_trace {

// Hooks the gear table rebuild (0x140c4eba0), CR15NetBalanceSettingsCS's init (0x140cfe650) and teardown (0x140d4cd70),
// and the client's R15NetBalanceSettingsUpdate listener (0x140d49950). Each hook reads, forwards every call unchanged,
// and logs. A hook is installed only after its prologue bytes match; a mismatch is a Warning with the expected and
// actual bytes and that hook is skipped. Call after the hooking engine is up.
void Install(std::uintptr_t gameBase);

// From the NetGame state hook: logs the values in force at a session join or leave (and at every other transition).
void OnNetGameState(std::int32_t from, std::int32_t to);

// Forgets the startup values and the last record (tests).
void ResetForTest();

}  // namespace nevr_gear_trace
