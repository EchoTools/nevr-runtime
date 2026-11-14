// The owned-removal guard on Win32 (core/owned_remove.h, owned_remove_win32.cpp): the cases the POSIX test
// (src/quest/tests/owned_remove_test.cpp) runs, with CreateDirectoryA and CreateSymbolicLinkA. A symbolic link needs a
// privilege that not every account holds; a test that cannot make its link is skipped in its own body, not passed. Built
// only; not run in this change. Each test removes what it made through the guard itself.

#include <windows.h>

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/owned_remove.h"

namespace {

using nevr_owned::Outcome;
using nevr_owned::RemoveEntry;
using nevr_owned::RemoveOwned;
using nevr_owned::Result;

bool Exists(const std::string& path) { return ::GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

void WriteText(const std::string& path, const char* text) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  ASSERT_NE(f, nullptr);
  std::fwrite(text, 1, std::strlen(text), f);
  std::fclose(f);
}

std::string ReadText(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return std::string();
  std::string out;
  char buf[256];
  std::size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

// NEVR_TEST_RUN_ROOT when it is set, else the temp folder; no trailing backslash.
std::string BaseFolder() {
  char env[MAX_PATH] = {};
  if (::GetEnvironmentVariableA("NEVR_TEST_RUN_ROOT", env, MAX_PATH) > 0 && env[0] != '\0') return env;
  char temp[MAX_PATH] = {};
  if (::GetTempPathA(MAX_PATH, temp) == 0) return std::string();
  std::string base(temp);
  while (!base.empty() && (base.back() == '\\' || base.back() == '/')) base.pop_back();
  return base;
}

// Makes a directory symbolic link from `link` to `target`. False with `*error` set when the link was not made.
bool TryDirLink(const std::string& link, const std::string& target, DWORD* error) {
  if (::CreateSymbolicLinkA(link.c_str(), target.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY) != 0) return true;
  *error = ::GetLastError();
  return false;
}

class OwnedRemoveWin32Test : public ::testing::Test {
 protected:
  void SetUp() override {
    base_ = BaseFolder();
    ASSERT_FALSE(base_.empty());
    static unsigned counter = 0;
    const std::string stamp = std::to_string(::GetCurrentProcessId()) + "-" + std::to_string(::GetTickCount()) + "-" +
                              std::to_string(++counter);
    run_name_ = "nevr-owned-run-" + stamp;
    canary_name_ = "nevr-owned-canary-" + stamp;
    ASSERT_TRUE(::CreateDirectoryA((base_ + "\\" + run_name_).c_str(), nullptr));
    ASSERT_TRUE(::CreateDirectoryA((base_ + "\\" + canary_name_).c_str(), nullptr));
    run_ = base_ + "\\" + run_name_;
    canary_ = base_ + "\\" + canary_name_;
    WriteText(canary_ + "\\keep", "keep");
  }

  void TearDown() override {
    // What the tests made, by name: the canary's keep, the canary, then the run root (kNotEmpty would show a leftover).
    RemoveOwned(canary_, {"keep"});
    RemoveEntry(base_, canary_name_);
    RemoveEntry(base_, run_name_);
  }

  std::string base_;
  std::string run_name_;
  std::string canary_name_;
  std::string run_;
  std::string canary_;
};

TEST_F(OwnedRemoveWin32Test, EmbeddedNulAndDotDotAreRefused) {
  std::string nul_dots("..", 2);
  nul_dots.push_back('\0');
  nul_dots += "/" + canary_name_ + "/keep";  // "/" so only the NUL can be the reason for the refusal
  EXPECT_EQ(RemoveEntry(run_, nul_dots).outcome, Outcome::kRefusedName);
  EXPECT_EQ(RemoveEntry(run_, "../" + canary_name_ + "/keep").outcome, Outcome::kRefusedName);
  EXPECT_EQ(ReadText(canary_ + "\\keep"), "keep");
  std::string nul_root = run_;
  nul_root.push_back('\0');
  nul_root += "junk";
  EXPECT_EQ(RemoveEntry(nul_root, "missing").outcome, Outcome::kRefusedName);
}

TEST_F(OwnedRemoveWin32Test, LinkedParentIsRefusedAndCanaryIsIntact) {
  DWORD link_error = 0;
  if (!TryDirLink(run_ + "\\lnk", canary_, &link_error)) {
    if (link_error == ERROR_PRIVILEGE_NOT_HELD) GTEST_SKIP() << "symbolic links need SeCreateSymbolicLinkPrivilege";
    ADD_FAILURE() << "CreateSymbolicLinkA failed: " << link_error;
    return;
  }
  EXPECT_EQ(RemoveEntry(run_, "lnk/keep").outcome, Outcome::kRefusedLink);
  EXPECT_EQ(ReadText(canary_ + "\\keep"), "keep");
  EXPECT_EQ(RemoveEntry(run_, "lnk").outcome, Outcome::kRemoved);
  EXPECT_EQ(ReadText(canary_ + "\\keep"), "keep");
}

TEST_F(OwnedRemoveWin32Test, InvalidNamesAreRefused) {
  const char* const names[] = {"..", "/abs", "a\\b", "C:x", "", "a//b", "a/./b", "a/"};
  for (const char* name : names) {
    EXPECT_EQ(RemoveEntry(run_, name).outcome, Outcome::kRefusedName) << name;
  }
  EXPECT_EQ(ReadText(canary_ + "\\keep"), "keep");
}

TEST_F(OwnedRemoveWin32Test, ListedFolderWithUnlistedFileIsNotEmpty) {
  ASSERT_TRUE(::CreateDirectoryA((run_ + "\\d").c_str(), nullptr));
  WriteText(run_ + "\\d\\extra", "unlisted");
  EXPECT_EQ(RemoveEntry(run_, "d").outcome, Outcome::kNotEmpty);
  EXPECT_TRUE(Exists(run_ + "\\d\\extra"));
  EXPECT_TRUE(Exists(run_ + "\\d"));
  ASSERT_TRUE(::DeleteFileA((run_ + "\\d\\extra").c_str()));
  EXPECT_EQ(RemoveEntry(run_, "d").outcome, Outcome::kRemoved);
}

TEST_F(OwnedRemoveWin32Test, LinkAsLastComponentIsRemovedAndTargetIsKept) {
  DWORD link_error = 0;
  if (!TryDirLink(run_ + "\\lk2", canary_, &link_error)) {
    if (link_error == ERROR_PRIVILEGE_NOT_HELD) GTEST_SKIP() << "symbolic links need SeCreateSymbolicLinkPrivilege";
    ADD_FAILURE() << "CreateSymbolicLinkA failed: " << link_error;
    return;
  }
  EXPECT_EQ(RemoveEntry(run_, "lk2").outcome, Outcome::kRemoved);
  EXPECT_FALSE(Exists(run_ + "\\lk2"));
  EXPECT_EQ(ReadText(canary_ + "\\keep"), "keep");
}

TEST_F(OwnedRemoveWin32Test, FileEmptyFolderAndMissingEntry) {
  WriteText(run_ + "\\f", "x");
  ASSERT_TRUE(::CreateDirectoryA((run_ + "\\e").c_str(), nullptr));
  EXPECT_EQ(RemoveEntry(run_, "f").outcome, Outcome::kRemoved);
  EXPECT_FALSE(Exists(run_ + "\\f"));
  EXPECT_EQ(RemoveEntry(run_, "e").outcome, Outcome::kRemoved);
  EXPECT_FALSE(Exists(run_ + "\\e"));
  EXPECT_EQ(RemoveEntry(run_, "missing").outcome, Outcome::kAbsent);
}

TEST_F(OwnedRemoveWin32Test, OwnedOrderChildrenBeforeParents) {
  ASSERT_TRUE(::CreateDirectoryA((run_ + "\\t").c_str(), nullptr));
  ASSERT_TRUE(::CreateDirectoryA((run_ + "\\t\\s").c_str(), nullptr));
  WriteText(run_ + "\\t\\s\\g", "g");
  WriteText(run_ + "\\t\\f", "f");
  const std::vector<Result> results = RemoveOwned(run_, {"t/f", "t/s/g", "t/s", "t"});
  ASSERT_EQ(results.size(), 4u);
  for (const Result& r : results) EXPECT_EQ(r.outcome, Outcome::kRemoved) << r.entry;
  EXPECT_FALSE(Exists(run_ + "\\t"));
}

}  // namespace
