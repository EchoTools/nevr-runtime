// The session's group and the groups the cache keeps, on the Quest host (core/map_groups.h; ADR 0048 decisions 15 and 16; ADR 0045
// decision 8). The rules are the core's: the group id of a session's game text, the nil group, the bound (the oldest groups go
// first, and an index record costs its share of the budget), the latest-first order and the channel's request hook.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "core/map_format.h"
#include "core/map_groups.h"
#include "quest/tests/test_check.h"

namespace {

namespace mg = nevr_maps;

// The game's group text (FormatGuid: upper case, Windows layout) and the id the service's group id text gives: the same characters
// with the letters in lower case.
constexpr const char* kGameGroup = "44332211-6655-8877-99AA-BBCCDDEEFFB0";
constexpr const char* kGroupId = "44332211-6655-8877-99aa-bbccddeeffb0";
constexpr const char* kNilGuid = "00000000-0000-0000-0000-000000000000";
// The record of a group named "1" with no maps: 28 (syntax) + 20 (digits) + 1 (the id).
constexpr std::uint64_t kOneCharRecordBytes = 49;
constexpr std::uint64_t kBudgetForTwentyRecords = 1000;  // 20 * 49 fits, 21 * 49 does not
constexpr std::size_t kRecordsKept = 20;

void TestTheGroupIdIsTheLowerCaseGameText() {
  std::string id;
  QCHECK(mg::GroupIdOfSession(kGameGroup, &id) == mg::SessionGroup::kOk);
  QCHECK(id == kGroupId);
  QCHECK(mg::GroupIdOfSession(kGroupId, &id) == mg::SessionGroup::kOk && id == kGroupId);
}

void TestTheNilGroupAndTheUnsafeTextsAreRefused() {
  std::string id = "untouched";
  QCHECK(mg::GroupIdOfSession(kNilGuid, &id) == mg::SessionGroup::kNil);
  QCHECK(id == "untouched");  // set only on kOk
  QCHECK(mg::GroupIdOfSession("", &id) == mg::SessionGroup::kNotPathSafe);
  QCHECK(mg::GroupIdOfSession("../../escape", &id) == mg::SessionGroup::kNotPathSafe);
  QCHECK(mg::GroupIdOfSession("44332211_6655-8877-99AA-BBCCDDEEFFB0", &id) == mg::SessionGroup::kNotPathSafe);
  QCHECK(mg::GroupIdOfSession(std::string(mg::kGroupIdMaxBytes + 1, 'a'), &id) == mg::SessionGroup::kNotPathSafe);
  QCHECK(mg::GroupIdOfSession(std::string(mg::kGroupIdMaxBytes, 'a'), &id) == mg::SessionGroup::kOk);
}

void TestTheRecordCostsItsShareOfTheBudget() {
  QCHECK(mg::GroupRecordBytes("1", {}) == kOneCharRecordBytes);
  QCHECK(mg::GroupRecordBytes("", {}) > 0);  // a group with no manifest still takes room in the index
  QCHECK(mg::GroupRecordBytes("1", {"ab"}) == kOneCharRecordBytes + mg::kGroupRecordQuotedKeyBytes + 2);
}

void TestAllGroupsAreKeptWhenTheyFit() {
  QCHECK(mg::KeptGroupCount({}, 0) == 0);
  QCHECK(mg::KeptGroupCount({10, 10, 10}, 30) == 3);
}

void TestTheLatestGroupIsKeptWhateverItCosts() {
  QCHECK(mg::KeptGroupCount({999}, 0) == 1);
}

void TestTheOldestGroupsAreDroppedFirst() {
  QCHECK(mg::KeptGroupCount({10, 10, 10}, 25) == 2);  // the third does not fit
  // The second does not fit, so the third, which would fit by itself, goes too: no older group is kept past one that does not fit.
  QCHECK(mg::KeptGroupCount({5, 100, 1}, 50) == 1);
  QCHECK(mg::KeptGroupCount({5, 5, 100, 1}, 20) == 2);
}

void TestTheRecordsAreBoundedByTheBudget() {
  std::vector<std::uint64_t> cost;
  for (int i = 0; i < 100; ++i) cost.push_back(kOneCharRecordBytes);  // groups with no manifest: the record is all they cost
  QCHECK(mg::KeptGroupCount(cost, kBudgetForTwentyRecords) == kRecordsKept);
}

struct Group {
  std::string id;
  std::uint64_t bytes = 0;
};

void TestTheRequestedGroupBecomesTheLatestWithItsFields() {
  std::vector<Group> groups = {{"a", 1}, {"b", 2}, {"c", 3}};
  mg::RememberLatestGroup(groups, "c");
  QCHECK(groups.size() == 3 && groups[0].id == "c" && groups[0].bytes == 3);
  QCHECK(groups[1].id == "a" && groups[2].id == "b");
  mg::RememberLatestGroup(groups, "z");
  QCHECK(groups.size() == 4 && groups[0].id == "z" && groups[0].bytes == 0);
  QCHECK(groups[1].id == "c" && groups[2].id == "a" && groups[3].id == "b");
}

std::vector<std::string> g_requested;
void RecordRequest(const std::string& group) { g_requested.push_back(group); }

void TestTheSessionGroupRecordNamesItsOutcomeAndWritesNoRemoteText() {
  const std::string prefix = "[NEVR.MAPS] session group ";
  QCHECK(mg::SessionGroupRecord(mg::SessionGroupRequest::kRequested, kGroupId) == prefix + "requested group=" + kGroupId);
  QCHECK(mg::SessionGroupRecord(mg::SessionGroupRequest::kNoRequestFn, kGroupId) == prefix + "no_channel group=" + kGroupId);
  QCHECK(mg::SessionGroupRecord(mg::SessionGroupRequest::kNil, "") == prefix + "nil group=none");
  QCHECK(mg::SessionGroupRecord(mg::SessionGroupRequest::kNotPathSafe, "../escape") == prefix + "not_path_safe group=none");
}

void TestASessionSuccessAsksForItsGroupThroughTheChannelsHook() {
  std::string id;
  mg::SetSessionGroupRequest(nullptr);
  QCHECK(mg::RequestSessionGroup(kGameGroup, &id) == mg::SessionGroupRequest::kNoRequestFn);
  QCHECK(id == kGroupId);  // the id is set whether or not a channel answers
  QCHECK(g_requested.empty());

  mg::SetSessionGroupRequest(&RecordRequest);
  QCHECK(mg::RequestSessionGroup(kGameGroup, &id) == mg::SessionGroupRequest::kRequested);
  QCHECK(g_requested == std::vector<std::string>{kGroupId});
  QCHECK(mg::RequestSessionGroup(kNilGuid, &id) == mg::SessionGroupRequest::kNil);
  QCHECK(mg::RequestSessionGroup("../escape", &id) == mg::SessionGroupRequest::kNotPathSafe);
  QCHECK(g_requested.size() == 1);  // a nil or unsafe group is never requested
  mg::SetSessionGroupRequest(nullptr);
}

}  // namespace

int main() {
  TestTheGroupIdIsTheLowerCaseGameText();
  TestTheNilGroupAndTheUnsafeTextsAreRefused();
  TestTheRecordCostsItsShareOfTheBudget();
  TestAllGroupsAreKeptWhenTheyFit();
  TestTheLatestGroupIsKeptWhateverItCosts();
  TestTheOldestGroupsAreDroppedFirst();
  TestTheRecordsAreBoundedByTheBudget();
  TestTheRequestedGroupBecomesTheLatestWithItsFields();
  TestTheSessionGroupRecordNamesItsOutcomeAndWritesNoRemoteText();
  TestASessionSuccessAsksForItsGroupThroughTheChannelsHook();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "map_groups_quest_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("map_groups_quest_test: all checks passed\n");
  return 0;
}
