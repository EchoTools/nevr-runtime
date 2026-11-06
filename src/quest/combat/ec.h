/* The EC client's contract with the modules that ride on it (#2187, docs/adr/0046). This header is the part of `ec` the other four modules of the unit
 * compile against; the module itself (ec.cpp) lands in the same unit. Namespace nevr_quest::combat::ec.
 *
 *   OnNetTick       called at the end of every recvfrom on the game's own socket, by the thread that received a datagram of the server on it (up
 *                   to 4 callbacks, in registration order). powerboost's link / unlink sends and ecfx's tick hang here.
 *   OnDroneKilled   the handler a first-time 'K' datagram (a drone's health ran out on the server) goes to, on the libr15 thread that called
 *                   recvfrom. dronekill registers it.
 */
#pragma once

#include <cstdint>

namespace nevr_quest::combat::ec {

using NetTickFn = void (*)() noexcept;
using DroneKilledFn = void (*)(std::uint32_t index) noexcept;

// False when the table is full or `tick` is null. Safe before ec is installed: the callback is kept and runs once the wrappers are in.
bool OnNetTick(NetTickFn tick) noexcept;
// Replaces the handler (null clears it).
void OnDroneKilled(DroneKilledFn handler) noexcept;

}  // namespace nevr_quest::combat::ec
