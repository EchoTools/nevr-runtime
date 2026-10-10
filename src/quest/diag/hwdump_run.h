#pragma once
// The hardware dump (#335), started by the sentinel's constructor sequence when the hwdump feature is on.

namespace nevr_quest::hwdump {

// Installs the libr15 hooks (before any libr15 code runs) and starts the detached dump thread. Returns true
// when the thread started; a refused hook install leaves that hook's fields saying so. Never throws.
bool StartHwDump() noexcept;

}  // namespace nevr_quest::hwdump
