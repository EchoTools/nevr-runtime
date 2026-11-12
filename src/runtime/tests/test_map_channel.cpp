// The custom-map channel on the PC (runtime/content/map_channel.h, CDN_DESIGN PC5b) with no game and no game file: a bucket
// published into a temp folder and served on loopback, the real Win32 env, cache and libcurl, the real map script
// provider and config alias table, a fake resource-set registry, and LOCAL .nevrmap files signed with a test dev key.
#include "runtime/tests/map_channel_test_fixture.h"

namespace {

// ---- the synthetic script DLL (test_map_scripts.cpp's stock shape: one export, setup_bindings, a ret) ----
constexpr std::uint32_t kNt = 0x40, kOpt = kNt + 24, kOptBytes = 240, kSections = kOpt + kOptBytes;
constexpr std::uint32_t kTextRva = 0x1000, kRdataRva = 0x2000, kRdataFile = 0x400, kExportBytes = 0x60;

void Put16(std::string* b, std::size_t at, std::uint16_t v) {
  (*b)[at] = static_cast<char>(v);
  (*b)[at + 1] = static_cast<char>(v >> 8);
}
void Put32(std::string* b, std::size_t at, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) (*b)[at + i] = static_cast<char>(v >> (8 * i));
}
void Put64(std::string* b, std::size_t at, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) (*b)[at + i] = static_cast<char>(v >> (8 * i));
}
void PutSection(std::string* b, int index, const char* name, std::uint32_t rva, std::uint32_t file, std::uint32_t flags) {
  const std::size_t at = kSections + 40u * index;
  std::memcpy(&(*b)[at], name, std::strlen(name));
  Put32(b, at + 8, 0x200);
  Put32(b, at + 12, rva);
  Put32(b, at + 16, 0x200);
  Put32(b, at + 20, file);
  Put32(b, at + 36, flags);
}

std::string BuildDll() {
  std::string b(0x600, '\0');
  Put16(&b, 0, 0x5a4d);
  Put32(&b, 0x3c, kNt);
  Put32(&b, kNt, 0x4550);
  Put16(&b, kNt + 4, 0x8664);
  Put16(&b, kNt + 6, 2);
  Put16(&b, kNt + 20, kOptBytes);
  Put16(&b, kNt + 22, 0x2022);
  Put16(&b, kOpt, 0x20b);
  Put32(&b, kOpt + 4, 0x200);
  Put32(&b, kOpt + 8, 0x200);
  Put32(&b, kOpt + 20, kTextRva);
  Put64(&b, kOpt + 24, 0x180000000ULL);
  Put32(&b, kOpt + 32, 0x1000);
  Put32(&b, kOpt + 36, 0x200);
  Put16(&b, kOpt + 40, 6);
  Put16(&b, kOpt + 48, 6);
  Put32(&b, kOpt + 56, 0x3000);
  Put32(&b, kOpt + 60, 0x200);
  Put16(&b, kOpt + 68, 2);
  Put16(&b, kOpt + 70, 0x0160);
  Put64(&b, kOpt + 72, 0x100000);
  Put64(&b, kOpt + 80, 0x1000);
  Put64(&b, kOpt + 88, 0x100000);
  Put64(&b, kOpt + 96, 0x1000);
  Put32(&b, kOpt + 108, 16);
  Put32(&b, kOpt + 112, kRdataRva);
  Put32(&b, kOpt + 116, kExportBytes);
  PutSection(&b, 0, ".text", kTextRva, 0x200, 0x20 | 0x20000000 | 0x40000000);
  PutSection(&b, 1, ".rdata", kRdataRva, kRdataFile, 0x40 | 0x40000000);
  b[0x200] = static_cast<char>(0xc3);
  const std::size_t e = kRdataFile;
  Put32(&b, e + 12, kRdataRva + 0x50);
  Put32(&b, e + 16, 1);
  Put32(&b, e + 20, 1);
  Put32(&b, e + 24, 1);
  Put32(&b, e + 28, kRdataRva + 0x28);
  Put32(&b, e + 32, kRdataRva + 0x30);
  Put32(&b, e + 36, kRdataRva + 0x38);
  Put32(&b, e + 0x28, kTextRva);
  Put32(&b, e + 0x2c, kTextRva);
  Put32(&b, e + 0x30, kRdataRva + 0x40);
  Put32(&b, e + 0x34, kRdataRva + 0x40);
  Put16(&b, e + 0x3a, 1);
  std::memcpy(&b[e + 0x40], "setup_bindings", 14);
  std::memcpy(&b[e + 0x50], "map.dll", 7);
  return b;
}

MapFiles MakeMap(const std::string& id, const std::string& version) {
  MapFiles m;
  m.id = id;
  m.version = version;
  m.level = EchoVR::CSymbol64Hash(id.c_str());
  m.set = "NEVRRES1 synthetic set of " + id + std::string(512, 'x');
  m.script = BuildDll();
  m.alias_key = nevr_config_alias::kArenaRulesKey;
  m.alias = EchoVR::CSymbol64Hash((id + "_arena_rules").c_str());
  auto member = [](const std::string& path, const char* role, const std::string& bytes) {
    return json{{"path", path},
                {"role", role},
                {"size", bytes.size()},
                {"sha256", Sha(bytes)},
                {"pieces", json::array({{{"sha256", Sha(bytes)}, {"size", bytes.size()}}})}};
  };
  json script = member("scripts/" + Hex16(kScriptHash) + ".dll", "script", m.script);
  script["hash"] = Hex64(kScriptHash);
  const json doc = {{"schema", "nevr-map-package"},
                    {"schema_version", 1},
                    {"id", id},
                    {"version", version},
                    {"platform", nevr_maps::kPlatformPc},
                    {"level", Hex64(m.level)},
                    {"level_name", id},
                    {"parent", "0x3f9915d3001dc28e"},
                    {"game_build", "echovr.exe test"},
                    {"requires_stock_manifest_sha256", json::array()},
                    {"stock_data_version", "1"},
                    {"source_sha256", Sha("source " + id)},
                    {"members", json::array({member("set/windows-x86_64.nevrres", "set", m.set), script})},
                    {"rekeyed", json::array()},
                    {"derived", json::array()},
                    {"standins", json::array()},
                    {"config_aliases", json::array({{{"key", Hex64(m.alias_key)},
                                                      {"key_name", "mp_arena_rules"},
                                                      {"alias", Hex64(m.alias)},
                                                      {"alias_name", id + "_arena_rules"}}})}};
  m.index = doc.dump();
  return m;
}

// A second rules pair under a key ConfigAlias does not allow, so Stage keeps 1 of the 2.
void AddRefusedAlias(MapFiles* m) {
  json doc = json::parse(m->index);
  const std::string key_name = "mp_combat_rules";
  const std::string alias_name = m->id + "_combat_rules";
  doc["config_aliases"].push_back({{"key", Hex64(EchoVR::CSymbol64Hash(key_name.c_str()))},
                                   {"key_name", key_name},
                                   {"alias", Hex64(EchoVR::CSymbol64Hash(alias_name.c_str()))},
                                   {"alias_name", alias_name}});
  m->index = doc.dump();
}


// IC-E2E in miniature: publish, fetch, verify, mount, serve the script, arm the alias; then revoke and withdraw.
TEST_F(MapChannel, APublishedMapIsFetchedMountedServedAndAliasedThenARevocationWithdrawsIt) {
  TestKey key(1);
  Bucket bucket{root_ + "/bucket", key};
  const MapFiles arena = MakeMap("arena1", "1.0.0");
  bucket.Publish(arena);
  bucket.Sign(1);
  FolderServer server(bucket.dir);
  nevr_maps::ChannelConfig config = CdnConfig(server.Base(), key);

  const nevr_maps::FetchReport fetched =
      nevr_maps::FetchAll(*env_, *fs_, config, {}, nullptr, "2026-11-11T00:00:00Z", Always());
  EXPECT_EQ(fetched.reason, "");
  EXPECT_EQ(fetched.maps_updated, 1u);

  ChannelMounter mounter(Fakes());
  const nevr_maps::BootReport boot = nevr_maps::BootMount(*env_, *fs_, config, mounter, nullptr, "2026-11-11T00:00:00Z");
  ASSERT_EQ(boot.mounted, std::vector<std::string>{"arena1"});
  EXPECT_TRUE(mounter.AnyScript());
  Expect(arena);
  for (const nevr_maps::MountedMap& m : boot.staged) {
    pins_.push_back({m.id, m.version, m.index_sha256, m.platform, m.source_sha256, m.title, m.level});
  }

  const MountOutcome mount = ApplyMount(Plan(), FakeReport());
  EXPECT_EQ(mount.mounted, std::vector<std::string>{"map:arena1"});
  EXPECT_TRUE(mount.withdrawn.empty());
  EXPECT_EQ(mount.armed, 1u);
  EXPECT_TRUE(Served("map:arena1"));
  nevr_config_alias::Reconcile(arena.level, {arena.level});
  EXPECT_EQ(nevr_config_alias::Resolve(arena.alias_key), arena.alias);
  ASSERT_EQ(nevr_map_session_pin::MountedPins().size(), 1u);
  EXPECT_EQ(nevr_map_session_pin::MountedPins()[0].id, "arena1");

  // Revocation: the next manifest revokes the id; the next run's boot refuses it, so nothing is served or aliased.
  bucket.Sign(2, {"arena1"});
  EXPECT_EQ(nevr_maps::FetchAll(*env_, *fs_, config, {}, nullptr, "2026-11-11T00:00:00Z", Always()).reason, "");
  ApplyMount(Plan(), "[]");
  nevr_config_alias::Reconcile(arena.level, {arena.level});
  for (const auto& [owner, handle] : registry_.taken) env_->CloseBlob(handle);
  registry_.taken.clear();
  maps::Forget();
  expected_.clear();
  pins_.clear();
  ChannelMounter next_run(Fakes());
  const nevr_maps::BootReport after = nevr_maps::BootMount(*env_, *fs_, config, next_run, nullptr, "2026-11-11T00:00:00Z");
  EXPECT_TRUE(after.mounted.empty());
  ASSERT_EQ(after.not_mounted.size(), 1u);
  EXPECT_EQ(after.not_mounted[0].second, "revoked");
  EXPECT_FALSE(Served("map:arena1"));
  EXPECT_EQ(nevr_config_alias::Resolve(arena.alias_key), arena.alias_key);
}

// IC-WITHDRAW in miniature: the registry's mount refuses the set, so the observer withdraws the script and the alias.
TEST_F(MapChannel, ASetTheMountRefusesServesNoScriptAndArmsNoAlias) {
  TestKey key(1);
  Bucket bucket{root_ + "/bucket", key};
  const MapFiles arena = MakeMap("arena1", "1.0.0");
  bucket.Publish(arena);
  bucket.Sign(1);
  FolderServer server(bucket.dir);
  const nevr_maps::ChannelConfig config = CdnConfig(server.Base(), key);
  ASSERT_EQ(nevr_maps::FetchAll(*env_, *fs_, config, {}, nullptr, "2026-11-11T00:00:00Z", Always()).reason, "");
  ChannelMounter mounter(Fakes());
  ASSERT_EQ(nevr_maps::BootMount(*env_, *fs_, config, mounter, nullptr, "2026-11-11T00:00:00Z").mounted.size(), 1u);
  Expect(arena);
  registry_.refuse_at_mount.insert("map:arena1");

  const MountOutcome mount = ApplyMount(Plan(), FakeReport());
  EXPECT_EQ(mount.withdrawn, std::vector<std::string>{"map:arena1"});
  EXPECT_FALSE(Served("map:arena1"));
  EXPECT_FALSE(HoldsAlias(nullptr, "map:arena1", arena.alias));
  nevr_config_alias::Reconcile(arena.level, {arena.level});
  EXPECT_EQ(nevr_config_alias::Resolve(arena.alias_key), arena.alias_key);
  EXPECT_TRUE(nevr_map_session_pin::MountedPins().empty());
}

std::vector<std::string> g_warnings;
VOID CaptureWarning(EchoVR::LogLevel level, const CHAR* message) {
  if (level == EchoVR::LogLevel::Warning) g_warnings.emplace_back(message);
}

// A set that holds its alias record under the registry's type (CJsonConfigCR) mounts and serves, but arms no alias,
// and says so: the stock rules are read with a [NEVR.MAPS] alias_missing warning, never silently.
TEST_F(MapChannel, AnAliasRecordOfTheWrongTypeArmsNothingAndWarns) {
  TestKey key(1);
  Bucket bucket{root_ + "/bucket", key};
  const MapFiles arena = MakeMap("wrongtype1", "1.0.0");
  bucket.Publish(arena);
  bucket.Sign(1);
  FolderServer server(bucket.dir);
  nevr_maps::ChannelConfig config = CdnConfig(server.Base(), key);
  config.subscribed = {arena.id};
  ASSERT_EQ(nevr_maps::FetchAll(*env_, *fs_, config, {}, nullptr, "2026-11-11T00:00:00Z", Always()).reason, "");
  ChannelMounter mounter(Fakes());
  ASSERT_EQ(nevr_maps::BootMount(*env_, *fs_, config, mounter, nullptr, "2026-11-11T00:00:00Z").mounted.size(), 1u);
  Expect(arena);
  registry_.alias_type = kStockJsonConfigCRWin10;

  g_warnings.clear();
  SetBootLogSink(&CaptureWarning);
  const MountOutcome mount = ApplyMount(Plan(), FakeReport());
  SetBootLogSink(nullptr);
  EXPECT_EQ(mount.mounted, std::vector<std::string>{"map:wrongtype1"});
  EXPECT_EQ(mount.armed, 0u);
  EXPECT_TRUE(Served("map:wrongtype1"));
  nevr_config_alias::Reconcile(arena.level, {arena.level});
  EXPECT_EQ(nevr_config_alias::Resolve(arena.alias_key), arena.alias_key);
  const std::string expected = "[NEVR.MAPS] config_alias refused=alias_missing map=wrongtype1 key=mp_arena_rules";
  EXPECT_EQ(std::count_if(g_warnings.begin(), g_warnings.end(),
                          [&](const std::string& line) { return line.find(expected) != std::string::npos; }),
            1);
}

// D8 (OI-4): a CDN map with two aliases, one refused, keeps nothing it registered on the PC either: its set is withdrawn
// from the registry, its script is not served, no alias is staged, and map_summary names it alias_partial.
TEST_F(MapChannel, ACdnMapWithOneOfTwoAliasesRefusedIsWithdrawnWholeAndTheSummarySaysWhy) {
  TestKey key(1);
  Bucket bucket{root_ + "/bucket", key};
  MapFiles arena = MakeMap("arena1", "1.0.0");
  AddRefusedAlias(&arena);
  bucket.Publish(arena);
  bucket.Sign(1);
  FolderServer server(bucket.dir);
  const nevr_maps::ChannelConfig config = CdnConfig(server.Base(), key);
  ASSERT_EQ(nevr_maps::FetchAll(*env_, *fs_, config, {}, nullptr, "2026-11-11T00:00:00Z", Always()).reason, "");

  ChannelMounter mounter(Fakes());
  g_warnings.clear();
  SetBootLogSink(&CaptureWarning);
  const nevr_maps::BootReport boot = nevr_maps::BootMount(*env_, *fs_, config, mounter, nullptr, "2026-11-11T00:00:00Z");
  SetBootLogSink(nullptr);
  EXPECT_TRUE(boot.mounted.empty());
  EXPECT_TRUE(boot.staged.empty());
  EXPECT_EQ(boot.not_mounted,
            (std::vector<std::pair<std::string, std::string>>{{"arena1", "alias_partial"}}));
  EXPECT_EQ(registry_.taken.count("map:arena1"), 0u);
  EXPECT_EQ(registry_.withdrawn.count("map:arena1"), 1u);
  EXPECT_FALSE(Served("map:arena1"));
  EXPECT_FALSE(nevr_config_alias::Global().AnyStaged());
  EXPECT_EQ(std::count_if(g_warnings.begin(), g_warnings.end(),
                          [](const std::string& line) {
                            return line.find("map_summary") != std::string::npos &&
                                   line.find("arena1:alias_partial") != std::string::npos;
                          }),
            1);

  // Nothing of it reaches the mount either: nothing is expected, mounted, armed or pinned.
  const MountOutcome mount = ApplyMount(Plan(), FakeReport());
  EXPECT_TRUE(mount.mounted.empty());
  EXPECT_EQ(mount.armed, 0u);
  EXPECT_TRUE(nevr_map_session_pin::MountedPins().empty());
}

// The same for a LOCAL map: MountLocal refuses it alias_partial and leaves nothing registered.
TEST_F(MapChannel, ALocalMapWithOneOfTwoAliasesRefusedIsWithdrawnWhole) {
  TestKey dev(7);
  MapFiles local = MakeMap("arena1", "0.0.1-local");
  AddRefusedAlias(&local);
  WriteFile(LocalDir() + "/dev_key.pub", dev.PublicKeyFile("EchoConstruct local test key"));
  WriteLocal(local, dev, "nevr-local arena1");
  const LocalScan scan = ScanLocal(*env_, *fs_, LocalDir(), LocalCache());
  ASSERT_EQ(scan.maps.size(), 1u);
  ASSERT_EQ(scan.maps[0].reason, "");
  ChannelMounter mounter(Fakes());
  EXPECT_EQ(MountLocal(*env_, *fs_, scan.maps[0], mounter), "alias_partial");
  EXPECT_EQ(registry_.taken.count("map:arena1"), 0u);
  EXPECT_EQ(registry_.withdrawn.count("map:arena1"), 1u);
  EXPECT_FALSE(Served("map:arena1"));
  EXPECT_FALSE(nevr_config_alias::Global().AnyStaged());
}

TEST_F(MapChannel, AnEmptyMountReportWithdrawsEveryOwner) {
  expected_ = {"map:a", "map:b"};
  const MountOutcome o = ApplyMount(Plan(), "[]");
  EXPECT_EQ(o.withdrawn, (std::vector<std::string>{"map:a", "map:b"}));
  EXPECT_EQ(ApplyMount(Plan(), "not json").withdrawn.size(), 2u);
}

TEST_F(MapChannel, ReportOwnersAreTheMapEntriesWithTheirLevels) {
  const auto owners = MountedMapOwners(
      R"([{"plugin":"EchoMapLoader","levels":["0x1"]},{"plugin":"map:x","levels":["0x00000000000000ff"]}])");
  ASSERT_EQ(owners.size(), 1u);
  EXPECT_EQ(owners[0].first, "map:x");
  EXPECT_EQ(owners[0].second, std::vector<std::uint64_t>{0xff});
}

// IC-LOCAL: the editor's signed .nevrmap mounts with no network, is served, and is never pinned to peers.
TEST_F(MapChannel, ALocalNevrmapSignedWithTheDevKeyMountsAndServesAndIsNotPinned) {
  TestKey dev(7);
  const MapFiles local = MakeMap("arena1", "0.0.1-local");
  WriteFile(LocalDir() + "/dev_key.pub", dev.PublicKeyFile("EchoConstruct local test key"));
  WriteLocal(local, dev, "nevr-local arena1");

  const LocalScan scan = ScanLocal(*env_, *fs_, LocalDir(), LocalCache());
  ASSERT_EQ(scan.reason, "");
  ASSERT_EQ(scan.maps.size(), 1u);
  ASSERT_EQ(scan.maps[0].reason, "");
  ChannelMounter mounter(Fakes());
  EXPECT_EQ(MountLocal(*env_, *fs_, scan.maps[0], mounter), "");
  Expect(local);
  const MountOutcome mount = ApplyMount(Plan(), FakeReport());
  EXPECT_EQ(mount.mounted, std::vector<std::string>{"map:arena1"});
  EXPECT_TRUE(Served("map:arena1"));
  nevr_config_alias::Reconcile(local.level, {local.level});
  EXPECT_EQ(nevr_config_alias::Resolve(local.alias_key), local.alias);
  EXPECT_TRUE(nevr_map_session_pin::MountedPins().empty());
}

TEST_F(MapChannel, ALocalNevrmapIsRefusedForAnotherKeyAnotherCommentOrOneChangedByte) {
  TestKey dev(7), other(9);
  WriteFile(LocalDir() + "/dev_key.pub", dev.PublicKeyFile("EchoConstruct local test key"));
  WriteLocal(MakeMap("by_other", "1"), other, "nevr-local by_other");
  WriteLocal(MakeMap("wrong_comment", "1"), dev, "nevr-local someone_else");
  const MapFiles changed = MakeMap("changed", "1");
  WriteLocal(changed, dev, "nevr-local changed");
  std::string bytes = ReadFile(LocalPath("changed"));
  bytes[bytes.size() / 2] ^= 1;
  WriteFile(LocalPath("changed"), bytes);

  const LocalScan scan = ScanLocal(*env_, *fs_, LocalDir(), LocalCache());
  ASSERT_EQ(scan.maps.size(), 3u);
  std::map<std::string, std::string> reasons;
  for (const LocalMap& m : scan.maps) reasons[m.id] = m.reason;
  EXPECT_EQ(reasons["by_other"].rfind("signature_", 0), 0u);
  EXPECT_EQ(reasons["wrong_comment"], "signature_comment");
  EXPECT_EQ(reasons["changed"].rfind("signature_", 0), 0u);
  EXPECT_NE(reasons["changed"], "signature_comment");
}

TEST_F(MapChannel, NoDevKeyMeansNoLocalSource) {
  EXPECT_EQ(ScanLocal(*env_, *fs_, LocalDir(), LocalCache()).reason, "absent");
  WriteFile(LocalDir() + "/dev_key.pub", "untrusted comment: x\nnot a key\n");
  EXPECT_EQ(ScanLocal(*env_, *fs_, LocalDir(), LocalCache()).reason, "bad_dev_key");
}

// Must-fail (CODE_AUDIT_NEVR1 M1): a member whose bytes are not the ones its signed package.json names is refused, even
// in a .nevrmap the dev key signed.
TEST_F(MapChannel, ATamperedLocalMemberIsRefusedThoughTheFileIsSigned) {
  TestKey dev(7);
  WriteFile(LocalDir() + "/dev_key.pub", dev.PublicKeyFile("EchoConstruct local test key"));
  MapFiles m = MakeMap("arena1", "1");
  m.script[m.script.size() - 1] ^= 1;  // the index keeps the original sha256
  WriteLocal(m, dev, "nevr-local arena1");
  const LocalScan scan = ScanLocal(*env_, *fs_, LocalDir(), LocalCache());
  ASSERT_EQ(scan.maps.size(), 1u);
  EXPECT_EQ(scan.maps[0].reason, "member_hash");
}

// Must-fail (M1): with a file of the extraction held open, so the folder cannot be replaced, the copy on disk is still
// hashed: a replaced script is refused, never mounted.
TEST_F(MapChannel, AHeldOpenCacheFileNeverSkipsVerification) {
  TestKey dev(7);
  WriteFile(LocalDir() + "/dev_key.pub", dev.PublicKeyFile("EchoConstruct local test key"));
  const MapFiles m = MakeMap("arena1", "1");
  WriteLocal(m, dev, "nevr-local arena1");
  ASSERT_EQ(ScanLocal(*env_, *fs_, LocalDir(), LocalCache()).maps.at(0).reason, "");

  const std::string script = CachedScript("arena1");
  std::string replaced = ReadFile(script);
  ASSERT_EQ(replaced, m.script);
  replaced[replaced.size() - 1] ^= 1;
  WriteFile(script, replaced);
  const std::string set = script.substr(0, script.find("/scripts/")) + "/set/windows-x86_64.nevrres";
  // As another running game holds it: read, shared for reading only, so nothing can delete the folder.
  const HANDLE hold = CreateFileW(Wide(set).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(hold, INVALID_HANDLE_VALUE);
  const LocalScan scan = ScanLocal(*env_, *fs_, LocalDir(), LocalCache());
  CloseHandle(hold);
  ASSERT_EQ(scan.maps.size(), 1u);
  // The unheld script was deleted with the rest of the folder, so the held set is all that survives.
  EXPECT_EQ(scan.maps[0].reason, "member_unreadable");
  ChannelMounter mounter(Fakes());
  EXPECT_NE(MountLocal(*env_, *fs_, scan.maps[0], mounter), "");
  EXPECT_FALSE(Served("map:arena1"));

  // Released, the next boot replaces the copy and mounts the signed bytes.
  const LocalScan healed = ScanLocal(*env_, *fs_, LocalDir(), LocalCache());
  ASSERT_EQ(healed.maps.at(0).reason, "");
  EXPECT_TRUE(healed.maps[0].imported);
  EXPECT_EQ(ReadFile(CachedScript("arena1")), m.script);
}

// Must-fail (M1, N1): the replaced script itself is held, so it survives the folder's removal; its bytes are hashed
// through the held copy and refused, never mounted.
TEST_F(MapChannel, AHeldReplacedCacheFileIsHashedAndRefused) {
  TestKey dev(7);
  WriteFile(LocalDir() + "/dev_key.pub", dev.PublicKeyFile("EchoConstruct local test key"));
  const MapFiles m = MakeMap("arena1", "1");
  WriteLocal(m, dev, "nevr-local arena1");
  ASSERT_EQ(ScanLocal(*env_, *fs_, LocalDir(), LocalCache()).maps.at(0).reason, "");

  const std::string script = CachedScript("arena1");
  std::string replaced = ReadFile(script);
  ASSERT_EQ(replaced, m.script);
  replaced[replaced.size() - 1] ^= 1;
  WriteFile(script, replaced);
  const std::string set = script.substr(0, script.find("/scripts/")) + "/set/windows-x86_64.nevrres";
  // Both members held, as a running game holds them, so every member survives and the tampered one is hashed.
  HANDLE holds[2];
  const std::string held[2] = {set, script};
  for (int i = 0; i < 2; ++i) {
    holds[i] = CreateFileW(Wide(held[i]).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(holds[i], INVALID_HANDLE_VALUE) << held[i];
  }
  const LocalScan scan = ScanLocal(*env_, *fs_, LocalDir(), LocalCache());
  EXPECT_EQ(ReadFile(script), replaced);
  CloseHandle(holds[0]);
  CloseHandle(holds[1]);
  ASSERT_EQ(scan.maps.size(), 1u);
  EXPECT_EQ(scan.maps[0].reason, "member_hash");
  ChannelMounter mounter(Fakes());
  EXPECT_NE(MountLocal(*env_, *fs_, scan.maps[0], mounter), "");
  EXPECT_FALSE(Served("map:arena1"));

  const LocalScan healed = ScanLocal(*env_, *fs_, LocalDir(), LocalCache());
  ASSERT_EQ(healed.maps.at(0).reason, "");
  EXPECT_TRUE(healed.maps[0].imported);
  EXPECT_EQ(ReadFile(CachedScript("arena1")), m.script);
}

// Must-fail (M4): the first boot verifies and hashes, a second boot with nothing changed hashes nothing and still
// mounts, and any change (a member's stamp, the .nevrmap's) makes the next boot verify again.
TEST_F(MapChannel, ASecondBootDoesNoRehashAndAChangeVerifiesAgain) {
  TestKey dev(7);
  WriteFile(LocalDir() + "/dev_key.pub", dev.PublicKeyFile("EchoConstruct local test key"));
  const MapFiles m = MakeMap("arena1", "1");
  WriteLocal(m, dev, "nevr-local arena1");
  const std::uint64_t nevrmap_bytes = ReadFile(LocalPath("arena1")).size();

  HashCountingEnv first(*env_);
  const LocalScan imported = ScanLocal(first, *fs_, LocalDir(), LocalCache());
  ASSERT_EQ(imported.maps.at(0).reason, "");
  EXPECT_TRUE(imported.maps[0].imported);
  EXPECT_GE(first.hashed, nevrmap_bytes + m.set.size() + m.script.size());

  HashCountingEnv second(*env_);
  const LocalScan reused = ScanLocal(second, *fs_, LocalDir(), LocalCache());
  ASSERT_EQ(reused.maps.at(0).reason, "");
  EXPECT_FALSE(reused.maps[0].imported);
  EXPECT_EQ(second.hashed, 0u);
  EXPECT_EQ(reused.maps[0].dir, imported.maps[0].dir);
  ChannelMounter mounter(Fakes());
  EXPECT_EQ(MountLocal(second, *fs_, reused.maps[0], mounter), "");
  Expect(m);
  EXPECT_EQ(ApplyMount(Plan(), FakeReport()).mounted, std::vector<std::string>{"map:arena1"});
  EXPECT_TRUE(Served("map:arena1"));
  // Served() stands for the game's load, so the run keeps the script held (withdraw_after_load) until it ends.
  ApplyMount(Plan(), "[]");
  for (const auto& [owner, handle] : registry_.taken) env_->CloseBlob(handle);
  registry_.taken.clear();
  maps::Forget();
  expected_.clear();

  WriteFile(CachedScript("arena1"), m.script);  // same bytes, a new file stamp
  HashCountingEnv third(*env_);
  const LocalScan member_moved = ScanLocal(third, *fs_, LocalDir(), LocalCache());
  ASSERT_EQ(member_moved.maps.at(0).reason, "");
  EXPECT_TRUE(member_moved.maps[0].imported);
  EXPECT_GE(third.hashed, nevrmap_bytes);

  WriteFile(LocalPath("arena1"), ReadFile(LocalPath("arena1")));
  const LocalScan nevrmap_moved = ScanLocal(*env_, *fs_, LocalDir(), LocalCache());
  ASSERT_EQ(nevrmap_moved.maps.at(0).reason, "");
  EXPECT_TRUE(nevrmap_moved.maps[0].imported);
}

// A stamp that moves between the scan and the mount refuses the mount before any grant.
TEST_F(MapChannel, AMemberChangedAfterTheScanIsRefusedAtTheMount) {
  TestKey dev(7);
  WriteFile(LocalDir() + "/dev_key.pub", dev.PublicKeyFile("EchoConstruct local test key"));
  const MapFiles m = MakeMap("arena1", "1");
  WriteLocal(m, dev, "nevr-local arena1");
  const LocalScan scan = ScanLocal(*env_, *fs_, LocalDir(), LocalCache());
  ASSERT_EQ(scan.maps.at(0).reason, "");
  WriteFile(CachedScript("arena1"), m.script);
  ChannelMounter mounter(Fakes());
  EXPECT_EQ(MountLocal(*env_, *fs_, scan.maps[0], mounter), "stamp");
  EXPECT_FALSE(Served("map:arena1"));
  EXPECT_TRUE(registry_.taken.empty());
}

TEST_F(MapChannel, ZipNamesThatLeaveTheFolderAndBadCrcsAreRefused) {
  std::vector<std::pair<std::string, std::string>> entries;
  std::string problem;
  ASSERT_TRUE(ReadZipString(Zip({{"package.json", "{}"}, {"set/a", std::string(5000, 'z')}}), &entries, &problem)) << problem;
  ASSERT_EQ(entries.size(), 2u);
  EXPECT_EQ(entries[1].second, std::string(5000, 'z'));
  entries.clear();
  EXPECT_FALSE(ReadZipString(Zip({{"../evil", "x"}}), &entries, &problem));
  EXPECT_EQ(problem, "name");
  std::string bad = Zip({{"package.json", "abc"}});
  bad[30 + 12] = 'X';  // the stored data's first byte
  EXPECT_FALSE(ReadZipString(bad, &entries, &problem));
  EXPECT_EQ(problem, "crc");
}

// IC-LOCAL precedence: under ECHOMAPS_TESTMAP=1 only .testmap counts, so the editor's deleted staged copy lets LOCAL win;
// a staged copy there makes EchoMapLoader win.
TEST_F(MapChannel, TheLoaderWinsOnlyWithTheCopyItLoadsThisRun) {
  LoaderFolders folders;
  folders.plugin_dll = base_ + L"\\plugins\\EchoMapLoader.dll";
  folders.loader_root = base_ + L"\\echomaps";
  folders.testmap = true;
  WriteFile(root_ + "/echomaps/maps/arena1.kfclvl", "kfclvl");
  EXPECT_TRUE(FindLoaderMaps(folders, {"arena1"}).empty()) << "no plugin, no loader";
  WriteFile(root_ + "/plugins/EchoMapLoader.dll", "dll");
  EXPECT_TRUE(FindLoaderMaps(folders, {"arena1"}).empty()) << "the staged copy was deleted";
  WriteFile(root_ + "/echomaps/maps/.testmap/arena1.kfclvl", "kfclvl");
  const auto found = FindLoaderMaps(folders, {"arena1", "other"});
  ASSERT_EQ(found.size(), 1u);
  EXPECT_EQ(found[0].first, "arena1");
  folders.testmap = false;
  EXPECT_EQ(FindLoaderMaps(folders, {"arena1"}).size(), 1u) << "outside a test run the loader root's maps count";
}

// The loader indexes one folder and nothing below or beside it: a copy in the game's own maps folder, a folder named
// like a map, or a .testmap copy outside a test run is not what EchoMapLoader loads, so the CDN or LOCAL copy stays.
TEST_F(MapChannel, OnlyTheOneFolderTheLoaderIndexesCounts) {
  LoaderFolders folders;
  folders.plugin_dll = base_ + L"\\plugins\\EchoMapLoader.dll";
  folders.loader_root = base_ + L"\\echomaps";
  WriteFile(root_ + "/plugins/EchoMapLoader.dll", "dll");
  WriteFile(Utf8(exe_dir_) + "/maps/arena1.kfclvl", "kfclvl");
  WriteFile(root_ + "/echomaps/maps/.testmap/arena1.kfclvl", "kfclvl");
  ASSERT_TRUE(CreateDirectoryW((base_ + L"\\echomaps\\maps\\arena2.kfclvl").c_str(), nullptr));
  EXPECT_TRUE(FindLoaderMaps(folders, {"arena1", "arena2"}).empty());
  folders.testmap = true;
  WriteFile(root_ + "/echomaps/maps/arena3.kfclvl", "kfclvl");
  const auto found = FindLoaderMaps(folders, {"arena1", "arena2", "arena3"});
  ASSERT_EQ(found.size(), 1u);
  EXPECT_EQ(found[0].first, "arena1");
  folders.loader_root.clear();
  EXPECT_TRUE(FindLoaderMaps(folders, {"arena1"}).empty()) << "no loader root, nothing loads";
}

// IC-LOCAL, both environment states, read as nevr_entry.cpp reads them. With ECHOMAPS_TESTMAP=1 the editor's "Test as
// CDN map (local)" deleted its staged .testmap copy, so a maps\<id>.kfclvl must not shadow the CDN or LOCAL copy.
TEST_F(MapChannel, TheLoaderFoldersComeFromTheEnvironmentAsTheLoaderReadsThem) {
  struct EnvRestore {
    ~EnvRestore() {
      SetEnvironmentVariableW(L"ECHOMAPS_ROOT", nullptr);
      SetEnvironmentVariableW(L"ECHOMAPS_TESTMAP", nullptr);
    }
  } restore;
  SetEnvironmentVariableW(L"ECHOMAPS_ROOT", nullptr);
  SetEnvironmentVariableW(L"ECHOMAPS_TESTMAP", nullptr);
  const std::wstring plugins = base_ + L"\\plugins";
  WriteFile(root_ + "/plugins/EchoMapLoader.dll", "dll");
  WriteFile(root_ + "/echomaps/maps/arena1.kfclvl", "kfclvl");

  LoaderFolders f = LoaderFoldersThisRun(base_, plugins);
  EXPECT_EQ(f.plugin_dll, plugins + L"\\EchoMapLoader.dll");
  EXPECT_EQ(f.loader_root, base_ + L"\\echomaps");
  EXPECT_FALSE(f.testmap);
  EXPECT_EQ(FindLoaderMaps(f, {"arena1"}).size(), 1u) << "unset: the loader loads maps\\, so it wins";

  ASSERT_TRUE(SetEnvironmentVariableW(L"ECHOMAPS_TESTMAP", L"1"));
  f = LoaderFoldersThisRun(base_, plugins);
  EXPECT_TRUE(f.testmap);
  EXPECT_TRUE(FindLoaderMaps(f, {"arena1"}).empty()) << "testmap with the staged copy deleted: the CDN or LOCAL copy loads";
  WriteFile(root_ + "/echomaps/maps/.testmap/arena1.kfclvl", "kfclvl");
  EXPECT_EQ(FindLoaderMaps(LoaderFoldersThisRun(base_, plugins), {"arena1"}).size(), 1u);

  for (const wchar_t* not_one : {L"0", L"true", L"01", L" 1"}) {
    ASSERT_TRUE(SetEnvironmentVariableW(L"ECHOMAPS_TESTMAP", not_one));
    EXPECT_FALSE(LoaderFoldersThisRun(base_, plugins).testmap) << Utf8(not_one);
  }

  ASSERT_TRUE(SetEnvironmentVariableW(L"ECHOMAPS_ROOT", (base_ + L"\\runner\\EchoMaps\\").c_str()));
  EXPECT_EQ(LoaderFoldersThisRun(base_, plugins).loader_root, base_ + L"\\runner\\EchoMaps");
  ASSERT_TRUE(SetEnvironmentVariableW(L"ECHOMAPS_ROOT", L"\\"));
  f = LoaderFoldersThisRun(base_, plugins);
  EXPECT_TRUE(f.loader_root.empty()) << "a root of separators only is the loader's empty root";
  EXPECT_TRUE(FindLoaderMaps(f, {"arena1"}).empty());
}

TEST(MapChannelSettings, NoSubscriptionIsNotConfiguredAndKeysAreChecked) {
  const Lookup none = [](const char*) { return std::string(); };
  EXPECT_EQ(ReadSettings(none, none, {}).reason, "not_configured");
  TestKey key(3);
  const std::string line = key.PublicKeyFile("k").substr(key.PublicKeyFile("k").find('\n') + 1);
  const Lookup csv = [&](const char* k) {
    if (std::strcmp(k, "maps_subscribed") == 0) return std::string("b, a,a");
    if (std::strcmp(k, "maps_trusted_keys") == 0) return line;
    if (std::strcmp(k, "maps_server_allow") == 0) return std::string("a@" + std::string(64, '0'));
    return std::string();
  };
  const Lookup value = [](const char* k) {
    return std::string(std::strcmp(k, "maps_base_url") == 0 ? "https://maps.example" : "");
  };
  const Settings s = ReadSettings(value, csv, {});
  EXPECT_EQ(s.reason, "");
  EXPECT_EQ(s.config.subscribed, (std::vector<std::string>{"a", "b"}));
  EXPECT_EQ(s.config.base_url, "https://maps.example/");
  ASSERT_EQ(s.config.keys.size(), 1u);
  EXPECT_FALSE(s.config.keys[0].official);
  EXPECT_EQ(ReadSettings(value, csv, {key.pin}).reason, "mixed_keys");
  const Lookup bad = [](const char* k) { return std::string(std::strcmp(k, "maps_trusted_keys") == 0 ? "nope" : ""); };
  EXPECT_EQ(ReadSettings(none, bad, {}).reason, "bad_key");
}

TEST(MapChannelSettings, TheOfficialKeyCanComeFromTheConfigAndIsNeverMixedWithAuthorKeys) {
  const TestKey official(4), author(5);
  const auto bare = [](const TestKey& k) {
    const std::string file = k.PublicKeyFile("k");
    return file.substr(file.find('\n') + 1);
  };
  std::string official_keys, trusted_keys;
  const Lookup csv = [&](const char* k) {
    if (std::strcmp(k, "maps_subscribed") == 0) return std::string("a");
    if (std::strcmp(k, "maps_official_keys") == 0) return official_keys;
    if (std::strcmp(k, "maps_trusted_keys") == 0) return trusted_keys;
    return std::string();
  };
  const Lookup value = [](const char* k) {
    return std::string(std::strcmp(k, "maps_base_url") == 0 ? "https://maps.example/" : "");
  };

  official_keys = bare(official);
  Settings s = ReadSettings(value, csv, {});
  EXPECT_EQ(s.reason, "");
  ASSERT_EQ(s.config.keys.size(), 1u);
  EXPECT_TRUE(s.config.keys[0].official);
  EXPECT_EQ(s.config.keys[0].key.key_id, official.pin.key.key_id);
  EXPECT_EQ(s.configured_official_keys.size(), 1u);
  EXPECT_TRUE(s.author_keys.empty());

  official_keys = official.PublicKeyFile("minisign public key");
  s = ReadSettings(value, csv, {});
  ASSERT_EQ(s.config.keys.size(), 1u) << "the whole .pub file, its comment line included, is accepted";
  EXPECT_EQ(s.config.keys[0].key.key_id, official.pin.key.key_id);

  trusted_keys = bare(author);
  EXPECT_EQ(ReadSettings(value, csv, {}).reason, "mixed_keys");
  official_keys.clear();
  EXPECT_EQ(ReadSettings(value, csv, {official.pin}).reason, "mixed_keys") << "a compiled-in key mixes the same way";
  s = ReadSettings(value, csv, {});
  EXPECT_EQ(s.reason, "") << "a test channel: only its test key, in maps.trusted_keys";
  ASSERT_EQ(s.config.keys.size(), 1u);
  EXPECT_FALSE(s.config.keys[0].official);

  official_keys = "nope";
  trusted_keys.clear();
  s = ReadSettings(value, csv, {});
  EXPECT_EQ(s.reason, "bad_key");
  EXPECT_EQ(s.detail, "maps.official_keys[0]");
}

TEST(MapChannelLock, ASecondHolderOfOneRootIsBusy) {
  std::unique_ptr<nevr_cargo::Env> env = nevr_cargo::MakeWinEnv("");
  const std::string root = "D:/nevr-map-channel-lock-test-" + std::to_string(GetCurrentProcessId());
  EXPECT_TRUE(TryLockRoot(*env, root));
  EXPECT_FALSE(TryLockRoot(*env, root)) << "a second holder, as another game process would be";
  bool from_another_thread = true;
  std::thread([&] { from_another_thread = TryLockRoot(*env, root); }).join();
  EXPECT_FALSE(from_another_thread);
  EXPECT_TRUE(TryLockRoot(*env, root + "-other"));
}

TEST(MapChannelWorker, ARequestRunsExactlyOneMorePassAndASecondOneWaitsTenMinutes) {
  std::atomic<std::uint64_t> now{1000};
  std::atomic<int> passes{0};
  FetchWorker worker([&](const std::function<bool()>&) { ++passes; return nevr_maps::FetchReport{}; }, [&] { return now.load(); });
  EXPECT_FALSE(worker.Request()) << "not started";
  worker.Start();
  while (worker.Passes() < 1) Sleep(1);
  EXPECT_TRUE(worker.Request());
  while (worker.Passes() < 2) Sleep(1);
  now += kExtraFetchGapMs - 1;
  EXPECT_FALSE(worker.Request());
  now += 1;
  EXPECT_TRUE(worker.Request());
  while (worker.Passes() < 3) Sleep(1);
  EXPECT_TRUE(worker.Stop(std::chrono::milliseconds(2000)));
  EXPECT_EQ(passes.load(), 3);
}

TEST(MapChannelWorker, AStopThatTimesOutLeavesTheWorkerAliveAndDoesNotCrash) {
  std::atomic<bool> unblock{false};
  std::atomic<bool> entered{false};
  // Leaked on purpose, as StopFetch leaves its worker: the blocked pass still runs on it.
  FetchWorker* worker = new FetchWorker(
      [&](const std::function<bool()>&) {
        entered = true;
        while (!unblock) Sleep(1);  // a Get that ignores keep_going until its transfer bound
        return nevr_maps::FetchReport{};
      },
      [] { return std::uint64_t{0}; });
  worker->Start();
  while (!entered) Sleep(1);  // Start does not wait for the thread: a Stop before the pass begins would just stop
  EXPECT_FALSE(worker->Stop(std::chrono::milliseconds(50)));
  unblock = true;
  for (int i = 0; i < 2000 && worker->Passes() == 0; ++i) Sleep(1);
  EXPECT_EQ(worker->Passes(), 1u);
}

// The client's game-loop return and PerformGracefulShutdown can both reach StopFetch: two Stops in flight at once must
// both see the worker end, and only one of them may join its thread.
TEST(MapChannelWorker, TwoStopsAtOnceBothSeeTheWorkerEndAndALaterStopReturnsAtOnce) {
  std::atomic<bool> unblock{false};
  FetchWorker worker(
      [&](const std::function<bool()>&) {
        while (!unblock) Sleep(1);
        return nevr_maps::FetchReport{};
      },
      [] { return std::uint64_t{0}; });
  worker.Start();
  std::atomic<int> stopped{0};
  std::thread first([&] { stopped += worker.Stop(std::chrono::milliseconds(5000)) ? 1 : 0; });
  std::thread second([&] { stopped += worker.Stop(std::chrono::milliseconds(5000)) ? 1 : 0; });
  Sleep(50);
  unblock = true;
  first.join();
  second.join();
  EXPECT_EQ(stopped.load(), 2);
  EXPECT_TRUE(worker.Stop(std::chrono::milliseconds(0)));
  EXPECT_EQ(worker.Passes(), 1u);
}

TEST(MapChannelConstants, TheRulesRecordTypeIsStockArenaRulesTypeAndTheSharedVectorsValue) {
  EXPECT_EQ(kAliasRecordType, kStockJsonResourceWin10);
  const json constants = json::parse(ReadFile(NEVR_MAPS_CONSTANTS));
  EXPECT_EQ(std::strtoull(constants.at("kAliasRecordType").get<std::string>().c_str(), nullptr, 16), kAliasRecordType);
}

}  // namespace
