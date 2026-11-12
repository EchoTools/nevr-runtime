// CDN_DESIGN 11's IC-E2E, IC-DELTA, IC-REPLAY, IC-WITHDRAW and IC-LOCAL against what EchoConstruct writes: its E1 .nevrmap
// exports, ec_publish's directory bucket and E6's LOCAL staging, read from <exe dir>\ic\. The bucket is served on loopback
// and goes through the real fetch, verify, cache and BootMount into the real script provider and config alias table,
// with test_map_channel's fake resource-set registry. Not a suite test: the caller stages <exe dir>\ic, and every test
// fails without it.
//   ic\channel.pub                      ec_publish keygen's public key
//   ic\v1\, ic\v2\<id>.windows-x86_64.nevrmap   E1's exports; v2 changes one texture record
//   ic\bucket\                          the bucket after every step (publish v1, publish v2, revoke), so every blob
//   ic\manifest1|2|3.signed             its manifest after each of those steps
//   ic\blobs1.txt, ic\added2.txt        the blobs after publish v1; the blobs publish v2 uploaded
//   ic\local\                           E6's <nevr root>\maps\local: the v1 package, its .minisig and dev_key.pub
#include "runtime/tests/map_channel_test_fixture.h"

#include "core/nevr_root.h"

namespace {

constexpr char kId[] = "ic_e2e";
constexpr char kOwner[] = "map:ic_e2e";
constexpr std::uint64_t kIcScript = 0x1ce2e5c121700001ULL;
constexpr char kManifestPath[] = "/bucket/v1/maps/manifest.signed";

std::string Slashes(std::string path) {
  std::replace(path.begin(), path.end(), '\\', '/');
  return path;
}

std::wstring ExeDir() {
  wchar_t exe[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring dir(exe, n);
  return dir.substr(0, dir.find_last_of(L"\\/"));
}

std::string NevrmapName() { return std::string(kId) + kLocalSuffix; }

std::string NowUtc() {
  SYSTEMTIME t;
  GetSystemTime(&t);
  char text[32];
  std::snprintf(text, sizeof text, "%04u-%02u-%02uT%02u:%02u:%02uZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                t.wSecond);
  return text;
}

std::set<std::string> Lines(const std::string& path) {
  std::set<std::string> out;
  std::istringstream in(ReadFile(path));
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty()) out.insert(line);
  }
  return out;
}

std::map<std::string, std::string> Members(const std::string& nevrmap) {
  std::vector<std::pair<std::string, std::string>> entries;
  std::string problem;
  EXPECT_TRUE(ReadZipString(ReadFile(nevrmap), &entries, &problem)) << nevrmap << ": " << problem;
  return {entries.begin(), entries.end()};
}

std::string FinalPath(std::int64_t held) {
  wchar_t path[1024];
  const DWORD n = GetFinalPathNameByHandleW(reinterpret_cast<HANDLE>(static_cast<intptr_t>(held)), path, 1024,
                                            FILE_NAME_NORMALIZED);
  return n == 0 || n >= 1024 ? std::string() : Utf8(std::wstring(path, n));
}

struct MountedFile {
  std::string kind, owner, sha256, path;
  std::uint64_t hash = 0;
};

// ChannelMounter, recording each file it grants (with the file its handle names) and each alias it stages.
class RecordingMounter : public nevr_maps::Mounter {
 public:
  explicit RecordingMounter(const Registry& registry) : inner_(registry) {}
  std::vector<MountedFile> files;
  std::vector<nevr_maps::ConfigAlias> aliases;

  nevr_maps::Grant Script(const std::string& owner, std::uint64_t hash, std::int64_t held,
                          const std::string& sha256) override {
    const std::string path = FinalPath(held);
    const nevr_maps::Grant grant = inner_.Script(owner, hash, held, sha256);
    if (grant.ok) files.push_back({"script", owner, sha256, path, hash});
    return grant;
  }
  nevr_maps::Grant Rekey(const std::string& owner, std::uint64_t hash, std::uint64_t stock_source,
                         const std::string& stock_sha256) override {
    return inner_.Rekey(owner, hash, stock_source, stock_sha256);
  }
  nevr_maps::Grant Set(const std::string& owner, std::int64_t held, const std::string& label, const std::string& sha256,
                       bool verified_stamp) override {
    const std::string path = FinalPath(held);
    const nevr_maps::Grant grant = inner_.Set(owner, held, label, sha256, verified_stamp);
    if (grant.ok) files.push_back({"set", owner, sha256, path, 0});
    return grant;
  }
  nevr_maps::Grant StageAliases(const std::string& owner, const std::string& id, std::uint64_t main_level,
                                const std::vector<nevr_maps::ConfigAlias>& staged) override {
    const nevr_maps::Grant grant = inner_.StageAliases(owner, id, main_level, staged);
    if (grant.ok) aliases.insert(aliases.end(), staged.begin(), staged.end());
    return grant;
  }
  nevr_maps::Grant DerivedScript(const std::string& owner, std::uint64_t hash, std::int64_t held,
                                 const std::string& sha256, const std::string& stock_sha256) override {
    return inner_.DerivedScript(owner, hash, held, sha256, stock_sha256);
  }
  void Withdraw(const std::string& owner) override { inner_.Withdraw(owner); }

 private:
  ChannelMounter inner_;
};

class MapChannelIc : public MapChannel {
 protected:
  void SetUp() override {
    // Everything this test writes and reads must be on the exe's own drive, the sandbox runner's subst root.
    wchar_t tmp[MAX_PATH];
    ASSERT_NE(GetTempPathW(MAX_PATH, tmp), 0u);
    const std::wstring exe_dir = ExeDir();
    ASSERT_EQ(towlower(tmp[0]), towlower(exe_dir[0])) << "TEMP is not on the exe's drive: run through the sandbox";
    MapChannel::SetUp();
    in_ = Slashes(Utf8(exe_dir)) + "/ic";
    ASSERT_TRUE(ParseKeyText(ReadFile(in_ + "/channel.pub"), &key_)) << "no ec_publish key at " << in_;
    for (int step = 1; step <= 3; ++step) {
      ASSERT_FALSE(ReadFile(Manifest(step)).empty()) << Manifest(step);
    }
    server_ = std::make_unique<FolderServer>(in_ + "/bucket");
    config_.dir = root_ + "/cache/maps";
    config_.base_url = server_->Base();
    config_.subscribed = {kId};
    config_.keys = {key_};
    map_.id = kId;
    map_.level = EchoVR::CSymbol64Hash(kId);
    map_.alias_key = nevr_config_alias::kArenaRulesKey;
    map_.alias = EchoVR::CSymbol64Hash((std::string(kId) + "_arena_rules").c_str());
  }
  void TearDown() override {
    server_.reset();
    MapChannel::TearDown();
  }

  std::string Manifest(int step) const { return in_ + "/manifest" + std::to_string(step) + ".signed"; }
  std::string Nevrmap(int version) const { return in_ + "/v" + std::to_string(version) + "/" + NevrmapName(); }

  // The bucket as it stood after `step`: the blobs are all there, and the manifest decides what is reachable.
  nevr_maps::FetchReport Fetch(int step) {
    WriteFile(in_ + kManifestPath, ReadFile(Manifest(step)));
    const nevr_maps::FetchReport report =
        nevr_maps::FetchAll(*env_, *fs_, config_, {}, nullptr, NowUtc(), Always());
    std::printf("[   IC   ] fetch manifest%d: reason=\"%s\" maps_updated=%zu pieces_fetched=%zu pieces_reused=%zu "
                "bytes_fetched=%llu\n",
                step, report.reason.c_str(), report.maps_updated, report.pieces_fetched, report.pieces_reused,
                static_cast<unsigned long long>(report.bytes_fetched));
    return report;
  }

  std::set<std::string> BlobGetsSince(std::size_t from) const {
    const std::vector<std::string> paths = server_->Paths();
    std::set<std::string> out;
    for (std::size_t i = from; i < paths.size(); ++i) {
      if (paths[i].rfind("blobs/", 0) == 0) out.insert(paths[i]);
    }
    return out;
  }

  nevr_maps::BootReport Boot(RecordingMounter& mounter) {
    const nevr_maps::BootReport boot = nevr_maps::BootMount(*env_, *fs_, config_, mounter, nullptr, NowUtc());
    if (!boot.mounted.empty()) Expect(map_);
    for (const nevr_maps::MountedMap& m : boot.staged) {
      pins_.push_back({m.id, m.version, m.index_sha256, m.platform, m.source_sha256, m.title, m.level});
    }
    return boot;
  }

  // A new process: nothing granted, served or armed, and the provider forgets what the game loaded and is configured
  // again, as SetPolicy does at every boot.
  void Restart() {
    ApplyMount(Plan(), "[]");
    nevr_config_alias::Reconcile(0, {});
    for (const auto& [owner, handle] : registry_.taken) env_->CloseBlob(handle);
    registry_.taken.clear();
    ConfigureScripts();
    expected_.clear();
    pins_.clear();
  }

  // The set and the script granted are the editor's member bytes, read back through the handles the mount holds, from
  // under `cache`; the one alias is mp_arena_rules -> <id>_arena_rules.
  void ExpectMountedFrom(const RecordingMounter& mounter, int version, const std::string& cache) {
    const std::map<std::string, std::string> members = Members(Nevrmap(version));
    const auto set = members.find("set/windows-x86_64.nevrres");
    const auto script = members.find("scripts/" + Hex16(kIcScript) + ".dll");
    ASSERT_TRUE(set != members.end() && script != members.end());
    ASSERT_EQ(mounter.files.size(), 2u);
    std::set<std::string> kinds;
    for (const MountedFile& f : mounter.files) {
      kinds.insert(f.kind);
      const std::string& want = f.kind == "set" ? set->second : script->second;
      EXPECT_EQ(f.owner, kOwner);
      EXPECT_EQ(f.sha256, Sha(want)) << f.kind;
      EXPECT_EQ(Sha(ReadFile(f.path)), Sha(want)) << f.kind << " at " << f.path;
      EXPECT_NE(Slashes(f.path).find(cache), std::string::npos) << f.path;
      if (f.kind == "script") {
        EXPECT_EQ(f.hash, kIcScript);
      }
      std::printf("[   IC   ] mounted %s %s sha256=%s bytes=%zu from %s\n", f.kind.c_str(), f.owner.c_str(),
                  f.sha256.c_str(), want.size(), f.path.c_str());
    }
    EXPECT_EQ(kinds, (std::set<std::string>{"script", "set"}));
    ASSERT_EQ(mounter.aliases.size(), 1u);
    EXPECT_EQ(mounter.aliases[0].key, map_.alias_key);
    EXPECT_EQ(mounter.aliases[0].alias, map_.alias);
    EXPECT_EQ(mounter.aliases[0].key_name, "mp_arena_rules");
    EXPECT_EQ(mounter.aliases[0].alias_name, std::string(kId) + "_arena_rules");
  }

  void ExpectServedAndArmed(bool pinned) {
    const MountOutcome mount = ApplyMount(Plan(), FakeReport());
    EXPECT_EQ(mount.mounted, std::vector<std::string>{kOwner});
    EXPECT_TRUE(mount.withdrawn.empty());
    EXPECT_EQ(mount.armed, 1u);
    EXPECT_TRUE(Served(kOwner, kIcScript));
    nevr_config_alias::Reconcile(map_.level, {map_.level});
    EXPECT_EQ(nevr_config_alias::Resolve(map_.alias_key), map_.alias);
    EXPECT_EQ(nevr_map_session_pin::MountedPins().size(), pinned ? 1u : 0u);
  }

  std::string in_;
  nevr_maps::PinnedKey key_;
  std::unique_ptr<FolderServer> server_;
  nevr_maps::ChannelConfig config_;
  MapFiles map_;
};

// IC-E2E: ec_publish's bucket on loopback; the fetch verifies and caches; BootMount mounts the editor's set and script
// from the cache; the script is served and the alias armed; a restart with the server gone mounts it again.
TEST_F(MapChannelIc, E2E_APublishedEditorMapIsFetchedVerifiedCachedAndMountedAndAgainAfterARestart) {
  const nevr_maps::FetchReport fetched = Fetch(1);
  EXPECT_EQ(fetched.reason, "");
  EXPECT_EQ(fetched.maps_updated, 1u);
  const std::set<std::string> published = Lines(in_ + "/blobs1.txt");
  for (const std::string& blob : BlobGetsSince(0)) EXPECT_EQ(published.count(blob), 1u) << blob;

  RecordingMounter mounter(Fakes());
  const nevr_maps::BootReport boot = Boot(mounter);
  ASSERT_EQ(boot.mounted, std::vector<std::string>{kId});
  ASSERT_EQ(boot.staged.size(), 1u);
  EXPECT_EQ(boot.staged[0].version, "1.0.0");
  ExpectMountedFrom(mounter, 1, "/cache/maps/");
  ExpectServedAndArmed(true);

  Restart();
  server_.reset();
  RecordingMounter again(Fakes());
  const nevr_maps::BootReport second = Boot(again);
  EXPECT_EQ(second.mounted, std::vector<std::string>{kId});
  ExpectMountedFrom(again, 1, "/cache/maps/");
  ExpectServedAndArmed(true);
}

// IC-DELTA: after v1, the v2 republish (one texture record changed) fetches exactly the blobs ec_publish uploaded for it,
// under 10% of the set's bytes, and the next boot mounts v2.
TEST_F(MapChannelIc, DELTA_ARepublishWithOneTextureChangedFetchesOnlyWhatWasUploaded) {
  ASSERT_EQ(Fetch(1).reason, "");
  const std::size_t before = server_->Paths().size();
  const nevr_maps::FetchReport delta = Fetch(2);
  EXPECT_EQ(delta.reason, "");
  EXPECT_EQ(delta.maps_updated, 1u);
  const std::set<std::string> fetched = BlobGetsSince(before), uploaded = Lines(in_ + "/added2.txt");
  EXPECT_EQ(fetched, uploaded);
  for (const std::string& blob : fetched) std::printf("[   IC   ] delta fetched %s\n", blob.c_str());

  const std::map<std::string, std::string> v2 = Members(Nevrmap(2));
  const auto set = v2.find("set/windows-x86_64.nevrres");
  ASSERT_TRUE(set != v2.end());
  const double share = static_cast<double>(delta.bytes_fetched) / static_cast<double>(set->second.size());
  std::printf("[   IC   ] delta bytes_fetched=%llu set_bytes=%zu share=%.4f\n",
              static_cast<unsigned long long>(delta.bytes_fetched), set->second.size(), share);
  EXPECT_LT(share, 0.10);

  RecordingMounter mounter(Fakes());
  const nevr_maps::BootReport boot = Boot(mounter);
  ASSERT_EQ(boot.mounted, std::vector<std::string>{kId});
  EXPECT_EQ(boot.staged[0].version, "1.0.1");
  ExpectMountedFrom(mounter, 2, "/cache/maps/");
  ExpectServedAndArmed(true);
}

// IC-REPLAY: once serial 3 (v2) is stored, the old serial-2 manifest is refused and v2 stays the mounted version.
TEST_F(MapChannelIc, REPLAY_AnOldManifestServedAgainIsRefusedAndTheNewerVersionStays) {
  ASSERT_EQ(Fetch(1).reason, "");
  ASSERT_EQ(Fetch(2).reason, "");
  const nevr_maps::FetchReport replay = Fetch(1);
  EXPECT_EQ(replay.reason, "serial_down");
  EXPECT_EQ(replay.maps_updated, 0u);

  RecordingMounter mounter(Fakes());
  const nevr_maps::BootReport boot = Boot(mounter);
  ASSERT_EQ(boot.mounted, std::vector<std::string>{kId});
  EXPECT_EQ(boot.staged[0].version, "1.0.1");
  ExpectMountedFrom(mounter, 2, "/cache/maps/");
}

// IC-WITHDRAW: `ec_publish revoke` of the id reaches the client, and the next boot mounts, serves and arms nothing.
TEST_F(MapChannelIc, WITHDRAW_ARevokedMapIsNotMountedServedOrArmedAtTheNextBoot) {
  ASSERT_EQ(Fetch(1).reason, "");
  RecordingMounter mounter(Fakes());
  ASSERT_EQ(Boot(mounter).mounted, std::vector<std::string>{kId});
  ExpectServedAndArmed(true);

  EXPECT_EQ(Fetch(3).reason, "");
  Restart();
  RecordingMounter next(Fakes());
  const nevr_maps::BootReport after = Boot(next);
  EXPECT_TRUE(after.mounted.empty());
  ASSERT_EQ(after.not_mounted.size(), 1u);
  EXPECT_EQ(after.not_mounted[0], (std::pair<std::string, std::string>{kId, "revoked"}));
  EXPECT_TRUE(next.files.empty());
  EXPECT_FALSE(Served(kOwner, kIcScript));
  nevr_config_alias::Reconcile(map_.level, {map_.level});
  EXPECT_EQ(nevr_config_alias::Resolve(map_.alias_key), map_.alias_key);
  EXPECT_TRUE(nevr_map_session_pin::MountedPins().empty());
}

// IC-LOCAL: E6's files under <nevr root>\maps\local mount as LOCAL: the same set and script, served and aliased, never
// pinned. The nevr root must be on the sandbox drive, never the real %LOCALAPPDATA%\nevr.
TEST_F(MapChannelIc, LOCAL_TheEditorsStagedLocalMapMountsAsLocalAndIsNotPinned) {
  const std::wstring nevr_root = nevr_root::RootW();
  ASSERT_FALSE(nevr_root.empty());
  ASSERT_EQ(towlower(nevr_root[0]), towlower(ExeDir()[0])) << "the nevr root is not on the sandbox drive";
  const std::string local = Slashes(Utf8(nevr_root)) + "/maps/local";
  for (const std::string& name : {NevrmapName(), NevrmapName() + ".minisig", std::string(kDevKeyFile)}) {
    const std::string bytes = ReadFile(in_ + "/local/" + name);
    ASSERT_FALSE(bytes.empty()) << name;
    WriteFile(local + "/" + name, bytes);
  }
  EXPECT_EQ(Sha(ReadFile(local + "/" + NevrmapName())), Sha(ReadFile(Nevrmap(1))));

  const LocalScan scan = ScanLocal(*env_, *fs_, local, local + ".cache");
  ASSERT_EQ(scan.reason, "");
  ASSERT_EQ(scan.maps.size(), 1u);
  ASSERT_EQ(scan.maps[0].reason, "");
  RecordingMounter mounter(Fakes());
  EXPECT_EQ(MountLocal(*env_, *fs_, scan.maps[0], mounter), "");
  Expect(map_);
  ExpectMountedFrom(mounter, 1, "/maps/local.cache/");
  ExpectServedAndArmed(false);
}

}  // namespace
