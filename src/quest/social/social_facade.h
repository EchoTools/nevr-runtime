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

#include <cstdint>
#include <memory>
#include <vector>

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
  // NRadEngine::SUuid::kInvalid (16 bytes), which ExitLobby stores. nullptr: sixteen zero bytes.
  const std::uint8_t* invalidUuid = nullptr;
};

// The process-wide models and SocialParty::Send.
Ports ProductionPorts();

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
