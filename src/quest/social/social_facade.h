// The Quest social facade: a CNSISocial-compatible object backed by the NEVR service instead of
// the Oculus platform.
//
// The game reaches friends, parties, invites and the recently-met list through one interface
// object, CNSISocial, which CNSProvider::Social() returns (docs/adr/0003 "Social provider").
// On the Quest store build that object is pnsovr's CNSOVRSocial, which fills it from the Oculus
// platform: the local member is the NEVR account id that the login rewrite hands the game, and
// every remote member is an Oculus org-scoped id. The facade is an object with the same vtable
// shape whose state is the shared PCVR social model (SocialParty / SocialRoster, the messages
// the NEVR service sends), so every id in party, room and friend flows is one NEVR account id.
//
// It is plain portable C++: the object is a block of bytes with a hand-built vtable of free
// functions, the same trick the PCVR facade uses, so the host tests drive it through the same
// function-pointer table the game uses. Every slot is noexcept: a failure inside one is logged,
// counted and answered with the slot's zero value, and never propagates into the game's frames.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "quest/social/social_abi.h"
#include "runtime/compat/social_party.h"
#include "runtime/compat/social_roster.h"

namespace quest_social {

// Everything outside the facade it talks to. Production wires the process-wide models and the
// bridge's sender; a test wires private ones and a recording sender.
struct Ports {
  SocialParty::State* party = nullptr;
  SocialRoster::Roster* friends = nullptr;
  SocialRoster::RecentList* recent = nullptr;
  // Frames and sends requests toward the NEVR service. Returns true only if all were accepted.
  // nullptr: nothing leaves (every request reports "NOT sent").
  bool (*send)(const std::vector<SocialParty::Message>& messages) = nullptr;
  // Monotonic seconds. nullptr: a steady clock.
  std::uint64_t (*nowSeconds)() = nullptr;
};

// The process-wide models and SocialParty::Send, returned by reference.
const Ports& ProductionPorts();

// The game's CJson functions (social_abi.h): Reset on the party and member CJson when the social object is reset,
// DecodeFrom to load the server's party and member data, EncodeToCompact to read out what the game wrote. The
// installer sets them once it has found libr15 of the pinned build (social_install.h); the default (all nullptr)
// loads and shares nothing, leaves the CJson alone and counts a Reset (jsonFailed). A test sets fakes.
void SetGameJson(const GameJson& json) noexcept;

// The load address of the pinned libpnsovr, set when the Social() hook selected the facade (0: not known). The facade's
// Initialize reads pnsovr's provider symbol from it and logs whether the game will derive the platform code the login
// carries (social_abi.h, "provider identity").
void SetPnsovrBias(std::uintptr_t loadBias) noexcept;

// A server frame of a social kind that changed nothing (unreadable, not an object, for another party): counted for
// the reporter (framesIgnored); the observer logs the reason on its own thread.
void NoteFrameIgnored() noexcept;

// What the facade counted, for the sentinel's reporter (RegisterSocialReportCounters). Zero is the
// healthy state for all of them except joinDeferred.
struct FacadeCounters {
  const std::atomic<std::uint64_t>& membersHidden;   // party members past the game's array: in the model, invisible to the game
  const std::atomic<std::uint64_t>& eventsDropped;   // callbacks lost because the carry queue was full
  const std::atomic<std::uint64_t>& sendFailed;      // requests the sender refused (state rolled back)
  const std::atomic<std::uint64_t>& joinDeferred;    // join attempts deferred behind a create or join in flight
  const std::atomic<std::uint64_t>& requestTimeout;  // create, join or lock requests the sender took and the server never answered
  // Callbacks delivered to the game, by class (the delivery itself never logs; the reporter does).
  const std::atomic<std::uint64_t>& cbCreated;      // PartyCreatedCB
  const std::atomic<std::uint64_t>& cbMemberJoined; // PartyMemberJoinedCB
  const std::atomic<std::uint64_t>& cbJoinFailed;   // PartyJoinFailedCB
  const std::atomic<std::uint64_t>& cbOther;        // every other callback: joined, updated, host changed, left, kicked, member updated/left, invite received, the accept gate
  const std::atomic<std::uint64_t>& jsonFailed;     // party or member data the game's JSON would not load, could not be read out, or was held for want of the game's functions; a Reset that could not call the game's CJson::Reset
  const std::atomic<std::uint64_t>& framesIgnored;  // server frames of a social kind that changed nothing
};
FacadeCounters FacadeCountersView() noexcept;
void ResetFacadeCountersForTest() noexcept;

// The signed-in account, in the NEVR id space the whole facade speaks: the id the login hands the game
// and the name to show for it. The login adapter calls it once the service accepts the login; until then
// the facade reports no local user and sends no party request. Safe from any thread.
void SetLocalAccount(std::uint64_t accountId, const char* displayName);

class Facade {
 public:
  explicit Facade(const Ports& ports);
  ~Facade();
  Facade(const Facade&) = delete;
  Facade& operator=(const Facade&) = delete;

  // The pointer the game receives from CNSProvider::Social(): an object of kObjectSize bytes
  // whose first word is the facade's vtable. Valid for the life of the Facade.
  void* Object() noexcept;

  // The process-wide facade (production ports), constructed on first use. Call it once from
  // an installer, outside any game frame, so the allocation happens before the hook is armed.
  static Facade& Instance();

  // 1 if the process-wide Instance() was ever destroyed; it never is.
  static std::uint32_t ProcessWideDestroyedCountForTest() noexcept;

  // Counters for tests and diagnostics.
  std::uint32_t InitializeCalls() const noexcept;
  std::uint32_t ShutdownCalls() const noexcept;
  std::uint32_t SlotFailures() const noexcept;  // slot calls that raised an exception
  std::uint32_t CallbackCalls() const noexcept; // callbacks delivered to the game

  struct Impl;  // opaque; named here so the slot functions in the .cpp can use it

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace quest_social
