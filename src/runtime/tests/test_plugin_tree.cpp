// test_plugin_tree.cpp — a plugin's dependency tree (ADR 0010, #741) over the fixture directories
// tools/gen_plugin_signature_fixtures.py writes into $NEVR_SIGFIX_DIR (see test_plugin_signature.cpp).
// ListDependencies is checked on the unsigned DLLs the generator builds; VerifyPluginTree on the signed trees.

#include "runtime/ext/plugin_tree.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace {

using nevr_plugin_signature::Dependencies;
using nevr_plugin_signature::Policy;
using nevr_plugin_signature::Reason;
using nevr_plugin_tree::HeldFiles;
using nevr_plugin_tree::TreeVerdict;

std::string WinPath(const std::string& p) {
  if (p.empty() || p[0] != '/') return p;
  std::string out = "Z:";
  for (char c : p) out += (c == '/') ? '\\' : c;
  return out;
}

std::vector<uint8_t> ReadBytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string s = ss.str();
  return {s.begin(), s.end()};
}

// The manifest names the reason as the enum spells it.
Reason ReasonFromName(const std::string& name) {
  if (name == "Unsigned") return Reason::Unsigned;
  if (name == "MissingEku") return Reason::MissingEku;
  if (name == "MalformedSignature") return Reason::MalformedSignature;
  if (name == "Unreadable") return Reason::Unreadable;
  ADD_FAILURE() << "the manifest names a reason this test does not know: " << name;
  return Reason::Accepted;
}

bool Has(const std::vector<std::string>& v, const std::string& name) {
  return std::find(v.begin(), v.end(), name) != v.end();
}

class PluginTree : public ::testing::Test {
 protected:
  void SetUp() override {
    const char* dir = std::getenv("NEVR_SIGFIX_DIR");
    ASSERT_NE(dir, nullptr) << "NEVR_SIGFIX_DIR is not set: run tools/gen_plugin_signature_fixtures.py <dir> "
                               "and export it (just test-auth-unit does)";
    dir_ = dir;
    std::ifstream in(dir_ + "/fixtures.json");
    ASSERT_TRUE(in.good()) << "no fixtures.json in " << dir_;
    manifest_ = nlohmann::json::parse(in);
    policy_.rootDer = ReadBytes(dir_ + "/" + manifest_["root_der"].get<std::string>());
    ASSERT_FALSE(policy_.rootDer.empty());
    policy_.profileEkus = {manifest_["profile_eku"].get<std::string>()};
    now_ = manifest_["now"].get<int64_t>();
  }

  HANDLE Open(const std::string& path) {
    HANDLE h = CreateFileA(WinPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_NE(h, INVALID_HANDLE_VALUE) << path << " error " << GetLastError();
    return h;
  }

  bool List(const std::string& file, Dependencies* deps, std::string* error) {
    HANDLE h = Open(dir_ + "/" + file);
    if (h == INVALID_HANDLE_VALUE) return false;
    const bool ok = nevr_plugin_signature::ListDependencies(h, deps, error);
    CloseHandle(h);
    return ok;
  }

  // Runs the tree walk the way the loader does: the plugin open, its directory with a trailing backslash.
  TreeVerdict Walk(const std::string& name, size_t* heldCount = nullptr) {
    const nlohmann::json& t = manifest_["trees"][name];
    const std::string directory = dir_ + "/" + t["dir"].get<std::string>() + "/";
    HANDLE root = Open(directory + t["root"].get<std::string>());
    if (root == INVALID_HANDLE_VALUE) return {};
    HeldFiles held;
    const TreeVerdict v = nevr_plugin_tree::VerifyPluginTree(WinPath(directory), root, policy_, now_, &held);
    if (heldCount != nullptr) *heldCount = held.size();
    CloseHandle(root);
    return v;
  }

  // The refusal the manifest says a correct walk makes, checked in full: which file, how it was reached, and why.
  void ExpectRefused(const std::string& name) {
    const nlohmann::json& t = manifest_["trees"][name];
    const TreeVerdict v = Walk(name);
    EXPECT_FALSE(v.accepted) << name << " was accepted";
    EXPECT_EQ(v.file, t["refused_file"].get<std::string>()) << v.Describe();
    EXPECT_EQ(v.via, t["via"].get<std::string>()) << v.Describe();
    EXPECT_EQ(v.verdict.reason, ReasonFromName(t["reason"].get<std::string>())) << v.Describe();
  }

  std::string dir_;
  nlohmann::json manifest_;
  Policy policy_;
  int64_t now_ = 0;
};

TEST_F(PluginTree, ListsAnImport) {
  Dependencies deps;
  std::string error;
  ASSERT_TRUE(List("importer.dll", &deps, &error)) << error;
  EXPECT_TRUE(Has(deps.imports, "dep.dll"));
  EXPECT_TRUE(deps.delayImports.empty());
  EXPECT_TRUE(deps.forwarders.empty());
}

TEST_F(PluginTree, ListsADelayLoadImport) {
  Dependencies deps;
  std::string error;
  ASSERT_TRUE(List("delayer.dll", &deps, &error)) << error;
  EXPECT_TRUE(Has(deps.delayImports, "dep.dll"));
  EXPECT_FALSE(Has(deps.imports, "dep.dll"));
}

TEST_F(PluginTree, ListsAForwardedExport) {
  Dependencies deps;
  std::string error;
  ASSERT_TRUE(List("forwarder.dll", &deps, &error)) << error;
  EXPECT_TRUE(Has(deps.forwarders, "dep.dll"));
  EXPECT_FALSE(Has(deps.imports, "dep.dll"));
}

TEST_F(PluginTree, AnImportDirectoryOutsideTheImageCannotBeListed) {
  Dependencies deps;
  std::string error;
  EXPECT_FALSE(List("broken.dll", &deps, &error));
  EXPECT_FALSE(error.empty());
}

TEST_F(PluginTree, SignedImportIsAccepted) {
  size_t held = 0;
  const TreeVerdict v = Walk("import_ok", &held);
  EXPECT_TRUE(v.accepted) << v.Describe();
  EXPECT_EQ(held, 1u);  // dep.dll stays open until the load is done
}

TEST_F(PluginTree, UnsignedImportIsRefused) { ExpectRefused("import_unsigned"); }
TEST_F(PluginTree, ImportSignedByAnotherSignerIsRefused) { ExpectRefused("import_wrong_signer"); }

// A name with no file in the plugins directory is the OS's only when the OS can supply it. Anything else is a name
// a file could be put under before the load, and it would map unchecked.
TEST_F(PluginTree, ImportWithNoFileInTheDirectoryAndNoneFromTheSystemIsRefused) {
  ExpectRefused("import_absent");
  EXPECT_NE(Walk("import_absent").verdict.detail.find("not supplied by the system"), std::string::npos);
}

// The loader appends ".dll" to a name with no extension, so "dep" maps dep.dll.
TEST_F(PluginTree, ImportNamedWithoutAnExtensionIsCheckedAsTheDllFile) { ExpectRefused("import_without_extension"); }

TEST_F(PluginTree, AbsentImportNamedWithoutAnExtensionIsRefusedUnderItsDllName) {
  ExpectRefused("import_without_extension_absent");
}

TEST_F(PluginTree, ImportOfASystemLibraryNeedsNoFileInTheDirectory) {
  char system[MAX_PATH] = {};
  ASSERT_GT(GetSystemDirectoryA(system, MAX_PATH), 0u);
  ASSERT_NE(GetFileAttributesA((std::string(system) + "\\hid.dll").c_str()), INVALID_FILE_ATTRIBUTES)
      << "System32 has no hid.dll: the fixture's system library is not under test";
  ASSERT_EQ(GetModuleHandleA("hid.dll"), nullptr) << "hid.dll is loaded: the System32 branch is not under test";
  const TreeVerdict v = Walk("import_system_without_extension");
  EXPECT_TRUE(v.accepted) << v.Describe();
}

TEST(PluginTreeNames, TheDefaultExtensionIsAppendedToANameWithNone) {
  EXPECT_EQ(nevr_plugin_tree::WithDefaultExtension(std::string("dep")), "dep.dll");
  EXPECT_EQ(nevr_plugin_tree::WithDefaultExtension(std::string("dep.dll")), "dep.dll");
  EXPECT_EQ(nevr_plugin_tree::WithDefaultExtension(std::string("dep.")), "dep.");  // a trailing dot is an extension
  EXPECT_EQ(nevr_plugin_tree::WithDefaultExtension(std::string("a.b\\dep")), "a.b\\dep.dll");  // the dot is in a directory
  EXPECT_EQ(nevr_plugin_tree::WithDefaultExtension(std::wstring(L"c:\\plugins\\late")), L"c:\\plugins\\late.dll");
  EXPECT_EQ(nevr_plugin_tree::WithDefaultExtension(std::wstring(L"late.bin")), L"late.bin");
}

TEST(PluginTreeNames, OnlyWhatTheSystemSuppliesNeedsNoFileInThePluginsDirectory) {
  EXPECT_TRUE(nevr_plugin_tree::SuppliedByTheSystem("api-ms-win-core-synch-l1-2-0.dll"));
  EXPECT_TRUE(nevr_plugin_tree::SuppliedByTheSystem("EXT-MS-WIN-FOO-L1-1-0.dll"));
  EXPECT_TRUE(nevr_plugin_tree::SuppliedByTheSystem("KERNEL32.DLL"));  // loaded, and in System32
  EXPECT_FALSE(nevr_plugin_tree::SuppliedByTheSystem("nevr-no-such-library.dll"));
  // A module already loaded satisfies an import by name, wherever its file is: this test's own executable.
  char exe[MAX_PATH] = {};
  ASSERT_GT(GetModuleFileNameA(nullptr, exe, MAX_PATH), 0u);
  const std::string exeName = std::string(exe).substr(std::string(exe).find_last_of("\\/") + 1);
  ASSERT_NE(GetModuleHandleA(exeName.c_str()), nullptr);
  EXPECT_TRUE(nevr_plugin_tree::SuppliedByTheSystem(exeName));
  // A directory of that name in System32 is not a library.
  char system[MAX_PATH] = {};
  ASSERT_GT(GetSystemDirectoryA(system, MAX_PATH), 0u);
  const DWORD drivers = GetFileAttributesA((std::string(system) + "\\drivers").c_str());
  ASSERT_TRUE(drivers != INVALID_FILE_ATTRIBUTES && (drivers & FILE_ATTRIBUTE_DIRECTORY) != 0)
      << "System32 has no drivers directory: this form is not under test";
  EXPECT_FALSE(nevr_plugin_tree::SuppliedByTheSystem("drivers"));
  EXPECT_FALSE(nevr_plugin_tree::SuppliedByTheSystem("dep.dll"));
}

TEST_F(PluginTree, SignedDelayLoadIsAccepted) {
  const TreeVerdict v = Walk("delay_ok");
  EXPECT_TRUE(v.accepted) << v.Describe();
}

TEST_F(PluginTree, UnsignedDelayLoadIsRefused) { ExpectRefused("delay_unsigned"); }

TEST_F(PluginTree, SignedForwarderTargetIsAccepted) {
  const TreeVerdict v = Walk("forwarder_ok");
  EXPECT_TRUE(v.accepted) << v.Describe();
}

TEST_F(PluginTree, UnsignedForwarderTargetIsRefused) { ExpectRefused("forwarder_unsigned"); }

TEST_F(PluginTree, SignedTransitiveTreeIsAccepted) {
  size_t held = 0;
  const TreeVerdict v = Walk("transitive_ok", &held);
  EXPECT_TRUE(v.accepted) << v.Describe();
  EXPECT_EQ(held, 2u);
}

TEST_F(PluginTree, UnsignedFileTwoLevelsDownIsRefused) { ExpectRefused("transitive_unsigned"); }

// The 64-file cap: 64 signed dependencies pass, the 65th is refused and the walk does not fail open.
TEST_F(PluginTree, SixtyFourSignedDependenciesAreAccepted) {
  size_t held = 0;
  const TreeVerdict v = Walk("cap_64", &held);
  EXPECT_TRUE(v.accepted) << v.Describe();
  EXPECT_EQ(held, 64u);
}

TEST_F(PluginTree, ASixtyFifthDependencyIsRefused) {
  ExpectRefused("cap_65");
  const TreeVerdict v = Walk("cap_65");
  EXPECT_NE(v.verdict.detail.find("over 64 files"), std::string::npos) << v.Describe();
}

TEST_F(PluginTree, ADependencyNamedByAPathIsRefused) {
  ExpectRefused("import_named_by_path");
  EXPECT_NE(Walk("import_named_by_path").verdict.detail.find("named by a path"), std::string::npos);
}

// Damaged tables: each file is a good DLL with one table broken. ListDependencies fails and says so, or (the
// overlapping directories) at least returns.
TEST_F(PluginTree, DamagedTablesAreRefusedWithAReason) {
  const nlohmann::json& corpus = manifest_["corpus"];
  ASSERT_GE(corpus.size(), 8u);
  for (auto it = corpus.begin(); it != corpus.end(); ++it) {
    if (it.key() == "import_named_by_path") continue;  // lists fine; the tree refuses it (above)
    const nlohmann::json& entry = it.value();
    Dependencies deps;
    std::string error;
    const bool ok = List(entry["file"].get<std::string>(), &deps, &error);
    if (entry["lists"].is_null()) continue;  // only that it returned
    EXPECT_EQ(ok, entry["lists"].get<bool>()) << it.key();
    if (!ok) {
      EXPECT_FALSE(error.empty()) << it.key();
      EXPECT_NE(error.find(entry["error_contains"].get<std::string>()), std::string::npos)
          << it.key() << ": " << error;
    }
  }
}

TEST_F(PluginTree, ADamagedTableInAPluginRefusesItInTheTreeWalk) {
  // The same files through the walk: a plugin whose table cannot be read is refused, never skipped.
  const nlohmann::json& corpus = manifest_["corpus"];
  for (auto it = corpus.begin(); it != corpus.end(); ++it) {
    if (it.value()["lists"].is_null() || it.value()["lists"].get<bool>()) continue;
    HANDLE h = Open(dir_ + "/" + it.value()["file"].get<std::string>());
    if (h == INVALID_HANDLE_VALUE) continue;
    HeldFiles held;
    // Unsigned, so a signature refusal would also end the walk: ListDependencies runs first on the root only
    // inside VerifyPluginTree after the root was verified by the loader; here the root is taken as given.
    const TreeVerdict v = nevr_plugin_tree::VerifyPluginTree(WinPath(dir_ + "/"), h, policy_, now_, &held);
    CloseHandle(h);
    EXPECT_FALSE(v.accepted) << it.key();
    EXPECT_NE(v.Describe().find("cannot list its dependencies"), std::string::npos) << it.key() << ": " << v.Describe();
  }
}

TEST_F(PluginTree, PluginWhoseDependenciesCannotBeListedIsRefused) {
  ExpectRefused("malformed");
  EXPECT_NE(Walk("malformed").Describe().find("cannot list its dependencies"), std::string::npos);
}

}  // namespace
