#pragma once

// How the three EchoCombat files reach each other. As plugins they found each other with GetModuleHandle and
// GetProcAddress ("VeilNet.dll", "VeilNet_DroneKilled", ...); compiled into the runtime they call these. Each file's
// Started() says whether it installed, which is what finding the DLL said before.

#include <cstdint>

namespace nevr_combat::dronefix {
/// Installs the drone follow crash guard (always) and, with `drone_on`, the DRONE rules. False when the guard's own
/// prologue check fails (then nothing is installed).
bool Start(bool drone_on);
void Stop();
bool Started();
/// The server says this drone was killed (VeilNet's 'K').
void Killed(std::uint32_t index);
}  // namespace nevr_combat::dronefix

namespace nevr_combat::powerboost {
/// OVERCHARGE; MEDIGUN's hooks only with `medigun_on`.
bool Start(bool is_server, bool medigun_on);
void Stop();
bool Started();
void MedigunRemote(std::uint32_t id);
std::uint64_t LocalWeapon();
}  // namespace nevr_combat::powerboost

namespace nevr_combat::veilnet {
/// UMBRAL VEIL and the combat net; the match control terminals with `match_control`; the server's MEDIGUN answer is
/// `medigun_server`.
bool Start(bool is_server, bool match_control, bool medigun_server);
/// The clean exit (Cat's TerminateProcess stub on the teardown call). True when it is in place.
bool StartCleanExit();
void Stop();
bool Started();
bool MatchControlStarted();
bool CleanExitInstalled();
void DroneKilled(std::uint32_t index);
void Medigun(std::uint32_t id);
int MedigunAllowed();
}  // namespace nevr_combat::veilnet
