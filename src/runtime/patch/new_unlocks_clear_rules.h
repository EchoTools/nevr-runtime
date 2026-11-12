#pragma once
// A "clear all new unlocks" made while a profile update is unacknowledged is sent after the acknowledgement (#2410). This header is the
// whole feature above the hooks: the layout it reads, when a clear is held, and when the dirty bit is set again. The hooks that feed it
// are patch/new_unlocks_clear.cpp on PC (and, later, the Quest build's own; both would supply the same addresses table below and call these
// rules); the PC GTest drives this one definition. The one hook is on the game's clear; the re-arm is a per-frame poll from the game loop.
//
// Evidence (ReVault, project echovr; PC VAs are echovr.exe, Quest VAs are libr15.so as the disassembly shows them):
//   - The game's clear (PC FUN_1401a06c0 0x1401a06c0; Quest CR15NetGame::RemoveAllNewlyUnlockedItems 0x1397114) returns at once when the
//     count at netGame+0x51c50 is 0; else it zeroes the count, sets the profile key "newunlocks" and sets bit 0 of the user's flag word at
//     user+0x9c (the dirty bit; user = *(netGame+0x28d0)).
//   - SaveClientProfileChanges returns unless that bit is set. The game has one copy of it in echovr.exe (PC 0x1406131f0, Quest 0x1a331f8) and
//     one in each platform module (pnsrad.dll, pnsovr.dll, pnsdemo.dll; the log lines "sent profile update" and "profile update succeeded" come
//     from the module that is live, pnsrad.dll on a client the runtime patches), and the echovr.exe copy is NOT the one a client runs (a detour
//     on it is never called in a client run). That is why nothing here hooks it: the dirty bit and the buffers are
//     plain fields of the user object, the same in every copy, so the poll below sets the bit itself. When the buffer at user+0x28 (the update
//     that was sent and not yet acknowledged) is not empty, it merges the pending changes into it, sends nothing, and clears the bit (PC
//     0x1406134dd). The acknowledgement handler (PC pnsrad.dll 0x180090300, Quest CNSUser::ProfileUpdateSuccessCB 0x1a34104) resets the
//     buffer at user+0x28 and sets the bit again only when the pending changes at user+0x18 are not empty. An add leaves entries there; the
//     clear leaves none, so a clear made in that window is merged, discarded and never sent (measured: scenario clear_while_update_in_flight,
//     3 runs; scenario clear_after_ack shows the send when nothing is in flight).
//   - "Not empty" is the game's own CJson::Empty (PC 0x1405f1360, Quest 0x1004730, a thunk): CJson_GetArraySize(json, "") == 0, the test
//     SaveClientProfileChanges uses on user+0x28 (PC 0x140613276). It has no side effect, so the glue can ask it whenever it likes.
//
// The fix keeps the game's own clear and its own send: the clear runs at once (the list empties on screen) and, when it ran while an
// update was in flight, each frame asks the game whether the buffer is empty yet and, once it is (the acknowledgement reset it), sets the
// dirty bit. Then the live SaveClientProfileChanges finds the buffer empty and sends the pending changes, which is what the game does when
// nothing is in flight (that send is `{}`, which the service stores as an empty
// list: server/evr_pipeline_login.go applyClientProfileUpdate, `profile.NewUnlocks = update.NewUnlocks`).
//
// Frequency (ADR 0038). The clear runs once per click. The poll runs once per frame and costs one pointer test while nothing is held; the game
// is asked about the buffer only for the held user.
//
// Threads: the clear hook and the poll run on the game loop's thread, so the state is not locked. A glue that cannot promise that takes the lock itself.

#include <cstddef>
#include <cstdint>

namespace nevr_new_unlocks_clear {

// CR15NetGame and CNSUser, the same on both builds (the Quest functions read the same fields: the ack handler adds +0x28 and +0x18,
// AddNewUnlocks reads +0x51c50 and +0x28d0 and +0x2da0, SaveClientProfileChanges tests +0x9c).
inline constexpr std::uintptr_t kNetGameUserOffset = 0x28D0;       // netGame -> CNSUser*
inline constexpr std::uintptr_t kNewUnlocksCountOffset = 0x51C50;  // netGame: how many new unlocks the client holds
inline constexpr std::uintptr_t kUserInFlightOffset = 0x28;        // CNSUser: the update sent and not yet acknowledged
inline constexpr std::uintptr_t kUserFlagsOffset = 0x9C;           // CNSUser: flag word
inline constexpr std::uint32_t kUserDirtyBit = 0x1;                // bit 0 of the flag word: the profile has changes to send

// What the build supplies per binary: the address of the one function it wraps (the clear) and of the game's emptiness test it calls (a plain list,
// read from the disassembly; each build checks the bytes at the address before it installs anything).
struct Addresses {
  std::uint64_t clear;       // the game's clear-all-new-unlocks: void(void* netGame)
  std::uint64_t jsonEmpty;   // CJson::Empty: nonzero when the json has nothing in it, int(void* json)
};
inline constexpr Addresses kPcAddresses = {0x1401A06C0ULL, 0x1405F1360ULL};
// Quest: ELF vaddrs of libr15.so (the build ID b243509c08ce677aeb95fa348016949b3fc45230 file; the disassembly lists each 0x100000 higher:
// the clear 0x1397114, CJson::Empty 0xfa82f8 is its thunk 0x1004730's target). `readelf -rW` on that file names these relocations, the words a
// GOT hook would patch: RemoveAllNewlyUnlockedItems JUMP_SLOT 0x36ccac0 and GLOB_DAT 0x3709e08; CJson::Empty JUMP_SLOT 0x36d9a80 (called, not
// hooked). Which copy of SaveClientProfileChanges a Quest client runs (libr15.so 0x19331f8 or a platform library's) is not measured. The Quest
// hook and frame poll that would use this table are not written yet (the sentinel's GOT hooks, pinned code words and host tests are their own unit).
inline constexpr Addresses kQuestAddresses = {0x1297114ULL, 0xFA82F8ULL};

// The words of the two log lines (logging.md, [NEVR.UNLOCKS]): the consequence is in the line, not only the event.
inline constexpr const char* kMsgHeld =
    "clear held: an update was unacknowledged, the game would merge this clear into it and drop it, so the clear is sent after the acknowledgement";
inline constexpr const char* kMsgRearmed = "clear re-armed: the update was acknowledged, the clear is sent now";
inline constexpr const char* kMsgSuperseded =
    "clear superseded: an add came after the clear, so that add's own update carries the list and the clear is not sent";
inline constexpr const char* kMsgDropped = "clear dropped: the user it was held for is gone or unreadable, so nothing is re-armed";

enum class ClearVerdict : std::uint8_t {
  kNothingCleared,  // the list was already empty: the game's clear returned at once
  kSentNormally,    // nothing was in flight: the game sends the clear itself
  kHeld,            // an update was in flight: the clear is re-armed after its acknowledgement
};

enum class PollVerdict : std::uint8_t {
  kNothingHeld,    // no clear is held for this user
  kSuperseded,     // held, but the list is not empty any more (an add came after the clear): the clear is dropped, the add's own update carries the list
  kStillInFlight,  // held, and the update is still unacknowledged
  kRearm,          // held, and the update was acknowledged: set the dirty bit now
};

class Hold {
 public:
  // After the game's clear returned. user is the CNSUser (compared, never dereferenced here), countBefore is the count at netGame+0x51c50
  // before the call, inFlight is "the buffer at user+0x28 is not empty" after it.
  ClearVerdict OnClear(const void* user, std::uint64_t countBefore, bool inFlight) {
    if (countBefore == 0) return ClearVerdict::kNothingCleared;
    if (!inFlight) {
      user_ = nullptr;  // the game sends this clear itself; an earlier held one is covered by it
      count_ = 0;
      return ClearVerdict::kSentNormally;
    }
    user_ = user;
    count_ = countBefore;
    return ClearVerdict::kHeld;
  }

  // The per-call gate: no game call is needed to know whether this user has a clear held.
  bool Holds(const void* user) const { return user_ != nullptr && user_ == user; }

  // Once per frame, for the held user. inFlight is "the buffer at user+0x28 is not empty" and countNow is the count at netGame+0x51c50 now; both
  // are only asked of a held user. A list that is not empty again (an add after the clear) is not what the clear said: the clear is dropped and
  // the update that add sent carries the list.
  PollVerdict OnPoll(const void* user, bool inFlight, std::uint64_t countNow) {
    if (!Holds(user)) return PollVerdict::kNothingHeld;
    if (countNow != 0) {
      Drop();
      return PollVerdict::kSuperseded;
    }
    if (inFlight) return PollVerdict::kStillInFlight;
    user_ = nullptr;
    return PollVerdict::kRearm;
  }

  // The user a clear is held for, or null: the poll's gate, so a frame with nothing held asks the game nothing.
  const void* HeldUser() const { return user_; }

  // Gives the held clear up (the user it was held for is gone); nothing is re-armed.
  void Drop() {
    user_ = nullptr;
    count_ = 0;
  }

  // How many new unlocks the held clear removed (for the log line); 0 when nothing is held.
  std::uint64_t HeldCount() const { return user_ != nullptr ? count_ : 0; }

  // The count of the clear that was just re-armed, for the line written after OnPoll returned kRearm.
  std::uint64_t LastCount() const { return count_; }

 private:
  const void* user_ = nullptr;
  std::uint64_t count_ = 0;
};

}  // namespace nevr_new_unlocks_clear
