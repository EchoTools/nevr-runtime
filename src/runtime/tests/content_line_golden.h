// The [NEVR.CONTENT] lines both builds write, pinned to one text (runtime/content/content_core.h, FormatDecision and
// FormatServed). The PC test (src/runtime/tests/test_content_pack.cpp) and the Quest test (src/quest/tests/content_folder_test.cpp)
// build the same inputs and compare the bytes with these, so the PC's lines and the Quest's are the same lines.
#pragma once

#include "runtime/content/content_core.h"

namespace content_golden {

inline constexpr const char* kDir = "ROOT/content";

// FormatDecision of ServedDecision().
inline constexpr const char* kServed =
    "[NEVR.CONTENT] content pack served id=echocombat version=1.0.0 manifests=1 packages=1 scripts=0 files_checked=2 "
    "dir=ROOT/content (the game reads these files in place of its stock ones; removing the pack's folder runs the game stock)";
// FormatDecision of a Decision with no pack.
inline constexpr const char* kNone =
    "[NEVR.CONTENT] no content pack installed dir=ROOT/content (the game runs on its stock data)";
// FormatDecision of RefusedDecision(): the detail's quote and newline are written as '?'.
inline constexpr const char* kRefused =
    "[NEVR.CONTENT] content pack refused id=echocombat reason=scripts_unsupported detail=\"a?b?\" dir=ROOT/content (nothing of the "
    "pack is served; the game runs on its stock data)";
// FormatServed of ServedManifest() for id "echocombat" and game path "GAME/manifests/48037dc70b0ecab2": the first serve, the
// second, and a serve the game's function failed with result 2.
inline constexpr const char* kFirstServe =
    "[NEVR.CONTENT] manifest served id=echocombat name=48037dc70b0ecab2 game_path=GAME/manifests/48037dc70b0ecab2 "
    "pack_file=PACK/manifests/48037dc70b0ecab2 count=1";
inline constexpr const char* kSecondServe =
    "[NEVR.CONTENT] manifest served id=echocombat name=48037dc70b0ecab2 game_path=GAME/manifests/48037dc70b0ecab2 "
    "pack_file=PACK/manifests/48037dc70b0ecab2 count=2";
inline constexpr const char* kFailedServe =
    "[NEVR.CONTENT] manifest open failed id=echocombat name=48037dc70b0ecab2 game_path=GAME/manifests/48037dc70b0ecab2 "
    "pack_file=PACK/manifests/48037dc70b0ecab2 result=0x2 (the game gets the open's failure)";
inline constexpr const char* kGamePath = "GAME/manifests/48037dc70b0ecab2";

inline nevr_content::Decision ServedDecision() {
  nevr_content::Decision d;
  d.outcome = nevr_content::Outcome::kServed;
  d.id = "echocombat";
  d.version = "1.0.0";
  d.manifests = 1;
  d.packages = 1;
  d.files_checked = 2;
  return d;
}

inline nevr_content::Decision RefusedDecision() {
  nevr_content::Decision d;
  d.outcome = nevr_content::Outcome::kRefused;
  d.id = "echocombat";
  d.reason = "scripts_unsupported";
  d.detail = "a\"b\n";
  return d;
}

inline void FillServedManifest(nevr_content::Served* s) {
  s->kind = nevr_content::Kind::kManifest;
  s->name = "48037dc70b0ecab2";
  s->key = "GAME/manifests/48037dc70b0ecab2";
  s->serve_path = "PACK/manifests/48037dc70b0ecab2";
  s->pack_file = "PACK/manifests/48037dc70b0ecab2";
}

}  // namespace content_golden
