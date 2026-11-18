// The dedicated server's maps hold, pure (core/map_server_hold.h; ADR 0048 decisions 13, 17 and 18): the timeout from the set's size
// and the measured rate and its bounds, the one group a server fetches, the hold's state over a fake clock (lifted by the mount,
// by the timeout, dropped by Clear), the composition with the levels hold, the signed-only rule and the lines. Plain main, the
// Quest host's own assertions; no game, no Win32, no Log().
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "core/map_server_hold.h"
#include "quest/tests/test_check.h"

namespace {

using nevr_maps::ComposeJoinHold;
using nevr_maps::HoldPlan;
using nevr_maps::JoinHold;
using nevr_maps::ServerMapHold;

constexpr std::uint64_t kMB = 1000000;
constexpr std::uint64_t kCeiling = 120000;
constexpr std::uint64_t kFloor = 5000;
constexpr std::uint64_t kSlack = 2000;

HoldPlan Plan(std::uint64_t set, std::uint64_t toFetch, std::uint64_t fetched, std::uint64_t elapsed) {
  HoldPlan p;
  p.planned = true;
  p.set_bytes = set;
  p.bytes_to_fetch = toFetch;
  p.bytes_fetched = fetched;
  p.elapsed_ms = elapsed;
  return p;
}

// The timeout (ADR 0048 decision 17, kfc spec 3b): the size and the rate, bounded by the floor and the ceiling, never zero.
void TestTimeoutFromSizeAndRate() {
  // 90 MB left at 10 MB in 2 s (5 MB/s): 18 s of transfer plus the 2 s slack.
  QCHECK(nevr_maps::MapHoldTimeoutMs(Plan(100 * kMB, 100 * kMB, 10 * kMB, 2000)) == 20000);
  // The part the cache holds is not waited for: 30 MB left, 6 s, plus the slack. A size mutant (set_bytes) would give 20000.
  QCHECK(nevr_maps::MapHoldTimeoutMs(Plan(100 * kMB, 40 * kMB, 10 * kMB, 2000)) == 8000);
  // Nothing left to download: the floor, never zero.
  QCHECK(nevr_maps::MapHoldTimeoutMs(Plan(100 * kMB, 0, 0, 0)) == kFloor);
  QCHECK(nevr_maps::MapHoldTimeoutMs(Plan(100 * kMB, 1000, 1000, 5)) == kFloor);
  // A small transfer: 500 bytes left at 500 bytes/s is 1 s, plus the 2 s slack is 3 s, under the floor, so the floor.
  QCHECK(nevr_maps::MapHoldTimeoutMs(Plan(2000, 1000, 500, 1000)) == kFloor);
  // Unknown plan, or no rate yet: the ceiling.
  HoldPlan unplanned;
  QCHECK(nevr_maps::MapHoldTimeoutMs(unplanned) == kCeiling);
  QCHECK(nevr_maps::MapHoldTimeoutMs(Plan(100 * kMB, 100 * kMB, 0, 0)) == kCeiling);
  QCHECK(nevr_maps::MapHoldTimeoutMs(Plan(100 * kMB, 100 * kMB, 0, 2000)) == kCeiling);
  QCHECK(nevr_maps::MapHoldTimeoutMs(Plan(100 * kMB, 100 * kMB, 10 * kMB, 0)) == kCeiling);
}

// The ceiling caps the timeout: a slow transfer of a large set, and the overflow of the rate's product, are the ceiling.
// A cap mutant (no clamp) gives a timeout past the ceiling.
void TestCeilingCapsAndOverflowIsTheCeiling() {
  QCHECK(nevr_maps::MapHoldTimeoutMs(Plan(1000 * kMB, 1000 * kMB, 1, 1000)) == kCeiling);
  QCHECK(nevr_maps::MapHoldTimeoutMs(Plan(0, std::numeric_limits<std::uint64_t>::max(), 1, 1000)) == kCeiling);
  QCHECK(nevr_maps::MapHoldTimeoutMs(
             Plan(0, std::numeric_limits<std::uint64_t>::max(), 1, std::numeric_limits<std::uint64_t>::max())) == kCeiling);
  // Under the ceiling it is kept as it is: 54 MB left at 1 MB/s is 54 s of transfer, and the slack makes 56 s.
  QCHECK(nevr_maps::MapHoldTimeoutMs(Plan(55 * kMB, 55 * kMB, kMB, 1000)) == 54000 + kSlack);
  // Every answer lies within the bounds.
  const HoldPlan cases[] = {Plan(1, 1, 1, 1), Plan(kMB, kMB, 1, 1), Plan(kMB, kMB, kMB / 2, 1), Plan(10 * kMB, 10 * kMB, 1, 1),
                            Plan(10 * kMB, 3, 2, 9)};
  for (const HoldPlan& c : cases) {
    const std::uint64_t t = nevr_maps::MapHoldTimeoutMs(c);
    QCHECK(t >= kFloor && t <= kCeiling);
  }
}

// Only the allocated group is fetched: the rule's one input is the allocation, so the hosted-guild list cannot add one.
void TestOnlyTheAllocatedGroupIsFetched() {
  QCHECK(nevr_maps::GroupToFetch("grp-a") == "grp-a");
  QCHECK(nevr_maps::GroupToFetch("grp-b") == "grp-b");  // a constant answer would fail one of the two
  QCHECK(nevr_maps::GroupToFetch("") == "");
}

// The hold: armed only for a group, lifted once by the mount of that group (another group's mount does nothing), once by the
// deadline, and dropped by Clear. Release at mount, release at timeout, and no release before either.
void TestHoldLiftedByMountOrTimeout() {
  ServerMapHold hold;
  QCHECK(!hold.Arm("", 0));  // no group, nothing held
  QCHECK(!hold.Holding());

  QCHECK(hold.Arm("grp", 1000));
  QCHECK(hold.Holding());
  QCHECK(hold.Mounted("other") == ServerMapHold::Step::kNone);  // another group's mount does not lift it
  QCHECK(hold.Holding());
  QCHECK(hold.Poll(1000 + kCeiling - 1) == ServerMapHold::Step::kNone);  // before the deadline it stands
  QCHECK(hold.Holding());
  QCHECK(hold.Mounted("grp") == ServerMapHold::Step::kMounted);  // released at mount
  QCHECK(!hold.Holding());
  QCHECK(hold.Mounted("grp") == ServerMapHold::Step::kNone);  // once
  QCHECK(hold.Poll(1000 + kCeiling) == ServerMapHold::Step::kNone);  // a lifted hold does not time out later

  // Released at timeout: the deadline is the armed time plus the timeout, computed from the plan the fetch noted.
  ServerMapHold timed;
  QCHECK(timed.Arm("grp", 0));
  timed.Note("grp", Plan(100 * kMB, 100 * kMB, 10 * kMB, 2000));  // 20 s
  QCHECK(timed.Poll(19999) == ServerMapHold::Step::kNone);
  QCHECK(timed.Holding());
  QCHECK(timed.Poll(20000) == ServerMapHold::Step::kTimeout);  // released at timeout
  QCHECK(!timed.Holding());
  QCHECK(timed.TimeoutMs() == 20000);
  QCHECK(timed.Group() == "grp");
  QCHECK(timed.Plan().set_bytes == 100 * kMB);
  QCHECK(timed.Plan().bytes_fetched == 10 * kMB);
  QCHECK(timed.Mounted("grp") == ServerMapHold::Step::kNone);  // a timed-out hold does not later report a mount

  // A note for another group is ignored; a clear drops the hold.
  ServerMapHold other;
  QCHECK(other.Arm("grp", 0));
  other.Note("elsewhere", Plan(100 * kMB, 100 * kMB, 10 * kMB, 2000));
  QCHECK(other.Poll(kCeiling) == ServerMapHold::Step::kTimeout);  // the plan stayed unknown: the ceiling
  QCHECK(other.TimeoutMs() == kCeiling);
  QCHECK(other.Arm("grp", 0));
  other.Clear();
  QCHECK(!other.Holding());
  QCHECK(other.Poll(kCeiling * 2) == ServerMapHold::Step::kNone);

  // A clock that goes back does not time the hold out.
  ServerMapHold back;
  QCHECK(back.Arm("grp", 5000));
  QCHECK(back.Poll(1000) == ServerMapHold::Step::kNone);
  QCHECK(back.Holding());
}

// The check at every session create: a group this process mounted is checked again, never skipped; a group it never mounted is
// fetched; no group is nothing. A "skip what is mounted" mutant answers kNone for the mounted group.
void TestEverySessionCreateChecksItsGroup() {
  using nevr_maps::SessionCheck;
  QCHECK(nevr_maps::SessionCheckFor("grp", false) == SessionCheck::kFetch);
  QCHECK(nevr_maps::SessionCheckFor("grp", true) == SessionCheck::kRecheck);
  QCHECK(nevr_maps::SessionCheckFor("", true) == SessionCheck::kNone);
  QCHECK(nevr_maps::SessionCheckFor("", false) == SessionCheck::kNone);
  // A check that cannot be made goes ahead only for a group already mounted.
  QCHECK(nevr_maps::FailedCheckGoesAhead(true));
  QCHECK(!nevr_maps::FailedCheckGoesAhead(false));
}

// A new version of a mounted map cannot be hosted in the process and the old one is never hosted: the hold stands past every
// deadline, for that group only, until the session ends (Clear). Only the mounted_other_version rows name the maps.
void TestAnUpdateOfAMountedMapHoldsEveryJoinUntilTheSessionEnds() {
  ServerMapHold hold;
  // K5 review finding 1: after an update, neither a mount nor a failed check (both call Mounted) lifts the hold.
  QCHECK(hold.Arm("grp", 0));
  QCHECK(hold.Updated("grp"));
  QCHECK(hold.Mounted("grp") == ServerMapHold::Step::kNone);  // a second fetch of the group mounts the old version: still held
  QCHECK(hold.Holding());
  QCHECK(hold.StandsForUpdate());
  QCHECK(hold.Mounted("grp") == ServerMapHold::Step::kNone);  // and again, as after a failed check
  QCHECK(hold.Holding());
  hold.Clear();
  QCHECK(!hold.Holding());
  QCHECK(!hold.StandsForUpdate());
  QCHECK(hold.Arm("grp", 0));  // the next session: a plain hold, lifted by its mount
  QCHECK(hold.Mounted("grp") == ServerMapHold::Step::kMounted);
  QCHECK(!hold.Holding());
  QCHECK(hold.Arm("grp", 1000));
  QCHECK(!hold.Updated("other"));  // another group's check does not change this hold
  QCHECK(!hold.StandsForUpdate());
  QCHECK(hold.Updated("grp"));
  QCHECK(hold.StandsForUpdate());
  QCHECK(hold.Poll(1000 + kCeiling * 10) == ServerMapHold::Step::kNone);  // no deadline
  QCHECK(hold.Holding());
  hold.Clear();  // the session ended
  QCHECK(!hold.Holding());
  QCHECK(!hold.StandsForUpdate());
  QCHECK(hold.Arm("grp", 0));  // the next session's hold has its deadline again
  QCHECK(hold.Poll(kCeiling) == ServerMapHold::Step::kTimeout);
  QCHECK(!ServerMapHold().Updated("grp"));  // nothing held: nothing to hold for an update

  const std::vector<std::pair<std::string, std::string>> rows = {
      {"arena_c", "mounted_other_version"}, {"beta", "not_cached"}, {"gamma", "stale"}, {"delta", "mounted_other_version"}};
  const std::vector<std::string> updated = nevr_maps::UpdatedWhileMounted(rows);
  QCHECK(updated.size() == 2 && updated[0] == "arena_c" && updated[1] == "delta");
  QCHECK(nevr_maps::UpdatedWhileMounted({{"beta", "not_cached"}}).empty());
  QCHECK(nevr_maps::UpdatedWhileMounted({}).empty());
}

// The lines of the check: the Info names the group and session, the update Warning the maps and what the server does, the failure
// Warning the consequence.
void TestCheckLines() {
  const std::string check = nevr_maps::MapCheckLine("grp", "sess");
  QCHECK(check.find("[NEVR.MAPS] map check group=grp session=sess (") == 0);
  QCHECK(nevr_maps::MapCheckLine("grp", "").find("session=none") != std::string::npos);
  const std::string update = nevr_maps::MapUpdateLine("grp", {"arena_c", "delta"});
  QCHECK(update.find("[NEVR.MAPS] map update group=grp ids=arena_c,delta reason=mounted_other_version") == 0);
  QCHECK(update.find("the joins wait until the server is restarted") != std::string::npos);
  QCHECK(nevr_maps::MapCheckFailedLine("grp").find("[NEVR.MAPS] map check failed group=grp (") == 0);
}

// A fetch that learns its size later moves the deadline: unknown at the ceiling, then the computed time, never past the ceiling.
void TestDeadlineFollowsThePlanWithinTheCeiling() {
  ServerMapHold hold;
  QCHECK(hold.Arm("grp", 0));
  QCHECK(hold.Poll(kFloor) == ServerMapHold::Step::kNone);  // no plan: the ceiling stands
  hold.Note("grp", Plan(100 * kMB, 100 * kMB, 10 * kMB, 2000));
  QCHECK(hold.Poll(20000) == ServerMapHold::Step::kTimeout);  // the plan's 20 s
  QCHECK(hold.TimeoutMs() == 20000);
}

// Composition with the levels hold (join_hold_ms): a join waits for the maps hold, and for the levels hold where it is on.
void TestComposeJoinHold() {
  // Maps holding: held, whatever the levels hold says (join_hold_ms=0 still holds).
  QCHECK(ComposeJoinHold(true, false, true, false) == JoinHold::kHeld);
  QCHECK(ComposeJoinHold(true, true, true, true) == JoinHold::kHeld);
  // Maps not holding: the levels hold alone. Off: released. On and settled: released. On, not settled, not timed out: held.
  QCHECK(ComposeJoinHold(false, false, false, false) == JoinHold::kSettled);
  QCHECK(ComposeJoinHold(false, true, true, false) == JoinHold::kSettled);
  QCHECK(ComposeJoinHold(false, true, false, false) == JoinHold::kHeld);
  QCHECK(ComposeJoinHold(false, true, false, true) == JoinHold::kTimeout);
}

// The signed-only rule for a server: the scripts mount only under an official signature when -require-signed-plugins is set.
void TestSignedOnlyRule() {
  QCHECK(nevr_maps::ServerMountsScripts(false, false));  // not signed-only: every manifest's scripts mount
  QCHECK(nevr_maps::ServerMountsScripts(false, true));
  QCHECK(!nevr_maps::ServerMountsScripts(true, false));  // signed-only, not official: refused
  QCHECK(nevr_maps::ServerMountsScripts(true, true));
  QCHECK(nevr_maps::SignedOnlyScriptsLine("grp", 0).empty());
  const std::string line = nevr_maps::SignedOnlyScriptsLine("grp", 2);
  QCHECK(line.find("[NEVR.MAPS] signed_only group=grp scripts_refused=2") == 0);
  QCHECK(line.find("script-free maps mount") != std::string::npos);
}

// The lines the glue writes: the Warning names the group, the set's size, the bytes fetched, the timeout and the consequence.
void TestLines() {
  const HoldPlan p = Plan(100 * kMB, 100 * kMB, 10 * kMB, 2000);
  const std::string warn = nevr_maps::MapHoldTimeoutLine("grp", p, 20000);
  QCHECK(warn.find("[NEVR.MAPS] map hold timeout group=grp set_bytes=100000000 bytes_fetched=10000000 timeout_ms=20000") == 0);
  QCHECK(warn.find("the session starts on stock content") != std::string::npos);
  QCHECK(nevr_maps::MapHoldArmedLine("grp", kCeiling).find("group=grp timeout_ms=120000") != std::string::npos);
  QCHECK(nevr_maps::MapHoldMountedLine("grp", 1500).find("group=grp waited_ms=1500 reason=mounted") != std::string::npos);
}

}  // namespace

int main() {
  TestTimeoutFromSizeAndRate();
  TestCeilingCapsAndOverflowIsTheCeiling();
  TestOnlyTheAllocatedGroupIsFetched();
  TestHoldLiftedByMountOrTimeout();
  TestDeadlineFollowsThePlanWithinTheCeiling();
  TestEverySessionCreateChecksItsGroup();
  TestAnUpdateOfAMountedMapHoldsEveryJoinUntilTheSessionEnds();
  TestCheckLines();
  TestComposeJoinHold();
  TestSignedOnlyRule();
  TestLines();
  const int failures = quest_test::Failures();
  std::printf("map_server_hold_test: %s (%d failure(s))\n", failures == 0 ? "OK" : "FAILED", failures);
  return failures == 0 ? 0 : 1;
}
