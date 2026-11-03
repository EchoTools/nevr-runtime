/* A measurement probe for #1737: how often the game calls the functions a combat reconstruction would replace, and what
 * its voice path reads from the microphone. It changes no game behaviour unless its microphone switch file is present.
 *
 * Built only with -DNEVR_QUEST_PERF_PROBE=ON (src/quest/CMakeLists.txt; default OFF), so no release or package-dev APK
 * carries it. Installed only on the pinned service-only libr15 (the reference base, AttachStatus::kPinnedServiceOnly):
 * every target is GotTarget::ServiceSide(), like path_rewrite_hook.h. On the store base the q15t plugin wraps two of the same
 * slots (CVR::HeadsetType, CR15NetGame::PartyMemberHeadsetType), and a probe installed first would make it refuse them.
 *
 * Six JUMP_SLOT imports of libr15.so (link-time slot addresses from `readelf -r` of the pinned file), each wrapped, timed
 * around the original call and counted:
 *   CNSProvider::MicAvailable(u64)            0x36e8fa0  read by CR15NetVoipBroadcasterCS::UpdateRecord (0x21c1178) once
 *   CNSProvider::MicRead(u64, short*, u64)    0x36e06e8  per game update while the voice broadcaster records
 *   CVR::HeadsetType()                        0x36f9fd8  callers split by return address: BindExpression<
 *                                                        CR15HeadsetTypeExpression> (0x202ba74, 184 bytes), CR15NetGame::
 *                                                        BotActorId(CSymbol64) (0x124dd38, 716 bytes), anything else
 *   CR15NetGame::PartyMemberHeadsetType(u32)  0x36fd880
 *   CR15NetPartyPermissionsExpression::operator()(SOutput&, SScriptThread&) const   0x36c6ee8
 *   CR15NetGame::BotActorId(CSymbol64) const  0x36c1878
 *
 * Microphone switch: a file `perf_probe_mic_available` in the sentinel's log directory (sentinel_log.h LogDir()) holding a
 * decimal sample count from 1 to 5760 makes MicAvailable answer that count instead of the provider's; absent, the provider's
 * answer passes through. Read once at install and logged either way.
 *
 * Log: one `perf_probe_install` line per slot and one `perf_probe` summary line at install. Then, per 10 s window in which
 * any probed function was called, written by the call that crosses the 10 s mark: one `perf_probe_calls` line at Info (the
 * counts, the mean and largest time spent in each original, and one timed dladdr of an address in the most recently loaded
 * script library: the per-call cost of the q15t link gate's library lookup), and, when the voice path read the microphone in
 * that window, one `perf_probe_mic` line at Info (the provider's answer, the answer given, and the read sizes by Opus frame).
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

namespace nevr_quest::perf_probe {

// Reads the microphone switch, resolves libr15's load base, then installs the six hooks. True when all six took.
bool InstallForServiceOnly() noexcept;

}  // namespace nevr_quest::perf_probe
