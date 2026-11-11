// A stand-in for the EC client's contract (src/quest/combat/ec.h) in the host tests of the modules that ride on it, for a program that does not
// link ec.cpp: it keeps what was registered, so a test can see the registration and call the handler as ec's recvfrom would. It also defines
// sentinel::EcPage() (ec_page.h) as plain storage, for a program that does not link ec_page.cpp (the drone explosion's page words).
#pragma once

#include "quest/combat/ec.h"

namespace ec_stub {

nevr_quest::combat::ec::DroneKilledFn DroneKilled() noexcept;
int NetTicks() noexcept;
void Reset() noexcept;

}  // namespace ec_stub
