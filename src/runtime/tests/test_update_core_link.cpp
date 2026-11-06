// The Rust core install method linked into a MinGW C++ binary and called from a thread that is not the main one, under Wine
// (docs/adr/0024-pc-self-update.md, "What is not known": "that the Rust core links into the MinGW runtime DLL and runs from a worker
// thread without a runtime of its own"). The same staticlib and the same system libraries nevr-runtime.dll links. What runs: the version
// string, a refused configuration, a manifest fetch from an empty folder, and a manifest fetch over https to a port nothing listens on
// (which initializes the TLS stack and the transport and must come back as a status, never a crash or a hang).

#include <gtest/gtest.h>

#include <windows.h>

#include <atomic>
#include <string>
#include <thread>

#include "installer/include/nevr_install.h"

namespace {

std::wstring TempDir(const wchar_t* name) {
  wchar_t temp[MAX_PATH];
  GetTempPathW(MAX_PATH, temp);
  const std::wstring dir = std::wstring(temp) + name + std::to_wstring(GetCurrentProcessId());
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

std::string Utf8(const std::wstring& w) {
  std::string out(static_cast<std::size_t>(WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr)), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), static_cast<int>(out.size()), nullptr, nullptr);
  return out;
}

std::string Escape(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '\\') out += "\\\\";
    else out += c;
  }
  return out;
}

}  // namespace

TEST(UpdateCoreLink, TheCoreAnswersFromAWorkerThread) {
  std::string version;
  int bad_config = -1;
  int file_fetch = -1;
  int https_fetch = -1;
  std::string file_error, https_error;
  const std::wstring root = TempDir(L"nevr-core-link-");
  const std::wstring empty = TempDir(L"nevr-core-link-base-");

  std::thread worker([&] {
    version = NevrCoreVersion();
    NevrSession* none = nullptr;
    bad_config = NevrInstallOpen("not json", &none);

    {
      const std::string cfg = "{\"platform\":\"pc\",\"nevr_root\":\"" + Escape(Utf8(root)) + "\",\"base\":\"file://" + Escape(Utf8(empty)) + "\"}";
      NevrSession* s = nullptr;
      if (NevrInstallOpen(cfg.c_str(), &s) == NEVR_OK && s != nullptr) {
        file_fetch = NevrManifestFetch(s);
        file_error = NevrLastError(s);
        NevrRunEnd(s, "unchanged", "test");
        NevrClose(s);
      }
    }
    {
      const std::string cfg = "{\"platform\":\"pc\",\"nevr_root\":\"" + Escape(Utf8(root)) + "\",\"base\":\"https://127.0.0.1:1/nevr-runtime\"}";
      NevrSession* s = nullptr;
      if (NevrInstallOpen(cfg.c_str(), &s) == NEVR_OK && s != nullptr) {
        https_fetch = NevrManifestFetch(s);
        https_error = NevrLastError(s);
        NevrRunEnd(s, "unchanged", "test");
        NevrClose(s);
      }
    }
  });
  worker.join();

  // NevrPcDiscard: the call the runtime makes when the verifier refuses a staged release; the core records it under the root.
  int discard = -1, bad_discard = -1;
  std::thread discarder([&] {
    const std::string cfg = "{\"platform\":\"pc\",\"nevr_root\":\"" + Escape(Utf8(root)) + "\"}";
    NevrSession* s = nullptr;
    if (NevrInstallOpen(cfg.c_str(), &s) == NEVR_OK && s != nullptr) {
      const std::string rootUtf8 = Utf8(root);
      const std::string sha(64, 'a');
      discard = NevrPcDiscard(s, rootUtf8.c_str(), "5.99.0", sha.c_str(), "verify_failed");
      bad_discard = NevrPcDiscard(s, rootUtf8.c_str(), "../x", sha.c_str(), "verify_failed");
      NevrRunEnd(s, "unchanged", "test");
      NevrClose(s);
    }
  });
  discarder.join();
  EXPECT_EQ(discard, NEVR_OK);
  EXPECT_EQ(bad_discard, NEVR_FAILED) << "a version outside the vocabulary is refused";
  EXPECT_NE(GetFileAttributesW((root + L"\\update\\manifest\\discards.json").c_str()), INVALID_FILE_ATTRIBUTES);

  EXPECT_FALSE(version.empty());
  EXPECT_EQ(bad_config, NEVR_FAILED) << "a configuration that is not JSON is refused with a status";
  EXPECT_EQ(file_fetch, NEVR_FAILED) << "an empty base has no manifest: " << file_error;
  EXPECT_FALSE(file_error.empty());
  EXPECT_EQ(https_fetch, NEVR_FAILED) << "nothing listens on port 1: " << https_error;
  EXPECT_FALSE(https_error.empty());
  // The core's own log is under the root it was given, in logs\ (ADR 0024, section 2): nothing is written beside the binary.
  EXPECT_NE(GetFileAttributesW((root + L"\\logs\\nevr-update.jsonl").c_str()), INVALID_FILE_ATTRIBUTES);
}
