#include "runtime/patch/broadcaster_allow_incoming.h"

#include "core/logging.h"
#include "runtime/server/broadcaster_config.h"

namespace nevr_broadcaster_allow_incoming {

BroadcasterInitializeFn OriginalBroadcasterInitialize = nullptr;

std::uint64_t BroadcasterInitializeHook(std::int64_t* self, std::uint64_t nameHash, std::int16_t port,
                                        std::uint32_t* mask, std::uint64_t p5, std::uint64_t p6, std::uint64_t p7,
                                        std::uint64_t p8, std::uint64_t p9, std::uint64_t p10, std::uint64_t p11,
                                        std::uint64_t p12, std::uint64_t p13, std::uint64_t p14, std::uint64_t p15) {
  const std::uint32_t maskIn = mask != nullptr ? *mask : 0u;
  const bool forced = nevr_broadcaster_config::ForceAllowIncoming(mask);
  const std::uint32_t maskOut = mask != nullptr ? *mask : 0u;
  Log(EchoVR::LogLevel::Info,
      "[NEVR.GAMESERVER] broadcaster initialize mask_in=0x%X mask_out=0x%X port=%u allow_incoming_forced=%s",
      static_cast<unsigned>(maskIn), static_cast<unsigned>(maskOut),
      static_cast<unsigned>(static_cast<std::uint16_t>(port)), forced ? "true" : "false");
  return OriginalBroadcasterInitialize(self, nameHash, port, mask, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14, p15);
}

}  // namespace nevr_broadcaster_allow_incoming
