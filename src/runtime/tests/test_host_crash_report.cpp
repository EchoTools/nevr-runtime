// The host's own crash report (runtime/host/crash_report_host.cpp, #1689): a crash the runtime is not there to report still
// leaves a readable report in the crash folder, written by the shared writer, which the next start's sender can send.
// nevr_host_core holds the production unit.

#include "runtime/host/crash_report_host.h"

#include "core/crash_send.h"

#include <gtest/gtest.h>

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

__declspec(noinline) std::uint64_t AddressInThisExecutable() {
  return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&AddressInThisExecutable)) + 4;
}

std::string ThisModuleName() {
  char path[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
  std::string full(path, length);
  const std::size_t slash = full.find_last_of("\\/");
  return slash == std::string::npos ? full : full.substr(slash + 1);
}

std::uint64_t ThisModuleBase() { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr))); }

class HostCrashReport : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = fs::temp_directory_path() / ("nevr-host-crash-report-test-" + std::to_string(GetCurrentProcessId()) + "-" +
                                         ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::error_code ec;
    fs::remove_all(root_, ec);
    fs::create_directories(root_);
    SetEnvironmentVariableW(L"LOCALAPPDATA", root_.wstring().c_str());
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(root_, ec);  // exactly the directory SetUp made
  }
  fs::path CrashDir() const { return root_ / "nevr" / "echovr" / "crashes"; }
  std::vector<fs::path> Reports() const {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::exists(CrashDir(), ec)) return out;
    for (const auto& entry : fs::directory_iterator(CrashDir(), ec)) {
      const std::string name = entry.path().filename().string();
      if (name.size() > 11 && name.compare(name.size() - 11, 11, nevr_crash::kReportSuffix) == 0) out.push_back(entry.path());
    }
    return out;
  }
  // An access violation at an address inside this executable, with a context captured here.
  struct Fault {
    CONTEXT context{};
    EXCEPTION_RECORD record{};
    EXCEPTION_POINTERS pointers{};
  };
  static void MakeFault(Fault* fault, std::uint64_t rip) {
    RtlCaptureContext(&fault->context);
    fault->context.Rip = rip;
    fault->record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    fault->record.ExceptionAddress = reinterpret_cast<PVOID>(static_cast<std::uintptr_t>(rip));
    fault->record.NumberParameters = 2;
    fault->record.ExceptionInformation[0] = 1;  // a write
    fault->record.ExceptionInformation[1] = 0x10;
    fault->pointers.ExceptionRecord = &fault->record;
    fault->pointers.ContextRecord = &fault->context;
  }
  fs::path root_;
};

}  // namespace

TEST_F(HostCrashReport, ACrashWithNoRuntimeLeavesAReportTheSenderReads) {
  nevr_host::PrepareCrashReport(nullptr);
  nevr_host::SnapshotModulesForCrashReport();
  Fault fault;
  const std::uint64_t rip = AddressInThisExecutable();
  MakeFault(&fault, rip);

  ASSERT_TRUE(nevr_host::WriteCrashReport(&fault.pointers));

  const std::vector<fs::path> reports = Reports();
  ASSERT_EQ(reports.size(), 1u);
  EXPECT_EQ(reports[0].filename().string().rfind("nevr-host-", 0), 0u);  // the host's own name, never the runtime's `nevr-crash-`
  std::string text;
  ASSERT_TRUE(nevr_crash_send::ReadFileBounded(reports[0], 64 * 1024, &text));
  const auto parsed = nevr_crash_send::ParseReport(text);
  ASSERT_TRUE(parsed.has_value()) << text;
  EXPECT_EQ(parsed->signalName, "EXCEPTION_ACCESS_VIOLATION");
  EXPECT_EQ(parsed->signalNumber, 0xC0000005);
  EXPECT_EQ(parsed->faultAddress, "0x0000000000000010");
  EXPECT_EQ(parsed->signalCode, 1);
  EXPECT_EQ(parsed->pc.module, ThisModuleName());  // resolved against the snapshot, not left as a raw address
  EXPECT_EQ(parsed->pc.offset.size() > 2, true);
  EXPECT_EQ(std::stoull(parsed->pc.offset, nullptr, 16), rip - ThisModuleBase());
  EXPECT_EQ(parsed->pc.buildId.rfind("pe-", 0), 0u);
  EXPECT_FALSE(parsed->lr.has_value());
  ASSERT_FALSE(parsed->backtrace.empty());
  EXPECT_EQ(parsed->backtrace[0].module, ThisModuleName());
}

TEST_F(HostCrashReport, ASenderPassTakesIt) {
  nevr_host::PrepareCrashReport(nullptr);
  nevr_host::SnapshotModulesForCrashReport();
  Fault fault;
  MakeFault(&fault, AddressInThisExecutable());
  ASSERT_TRUE(nevr_host::WriteCrashReport(&fault.pointers));

  static std::vector<std::string> bodies;
  bodies.clear();
  nevr_crash_send::Config config;
  config.dir = CrashDir();
  config.url = "https://service.invalid/api/v3/crash-reports";
  config.build = "5.7.1";
  config.platform = "pc";
  config.post = [](void*, const std::string&, const std::string& body, const nevr_crash_send::Timeouts&) {
    bodies.push_back(body);
    return nevr_crash_send::PostResult{true, 201, 0};
  };
  config.random = [](std::uint8_t* bytes, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) bytes[i] = static_cast<std::uint8_t>(i);
    return true;
  };
  EXPECT_EQ(nevr_crash_send::SendPending(config).sent, 1u);
  ASSERT_EQ(bodies.size(), 1u);
  EXPECT_NE(bodies[0].find(ThisModuleName()), std::string::npos);
  EXPECT_EQ(bodies[0].find("rax"), std::string::npos);  // the registers stay in the file
}

TEST_F(HostCrashReport, AnAddressInNoKnownModuleIsStillReported) {
  nevr_host::PrepareCrashReport(nullptr);
  nevr_host::SnapshotModulesForCrashReport();
  Fault fault;
  MakeFault(&fault, 0x1234);
  ASSERT_TRUE(nevr_host::WriteCrashReport(&fault.pointers));
  const std::vector<fs::path> reports = Reports();
  ASSERT_EQ(reports.size(), 1u);
  std::string text;
  ASSERT_TRUE(nevr_crash_send::ReadFileBounded(reports[0], 64 * 1024, &text));
  const auto parsed = nevr_crash_send::ParseReport(text);
  ASSERT_TRUE(parsed.has_value()) << text;
  EXPECT_EQ(parsed->pc.module, "unknown");
  EXPECT_EQ(parsed->pc.offset, "0x1234");
}

TEST_F(HostCrashReport, TwoReportsAreTwoFiles) {
  nevr_host::PrepareCrashReport(nullptr);
  nevr_host::SnapshotModulesForCrashReport();
  Fault fault;
  MakeFault(&fault, AddressInThisExecutable());
  ASSERT_TRUE(nevr_host::WriteCrashReport(&fault.pointers));
  ASSERT_TRUE(nevr_host::WriteCrashReport(&fault.pointers));
  EXPECT_EQ(Reports().size(), 2u);
}

TEST_F(HostCrashReport, WithoutAFolderNothingIsWrittenAndNothingThrows) {
  SetEnvironmentVariableW(L"LOCALAPPDATA", nullptr);
  nevr_host::PrepareCrashReport(nullptr);
  Fault fault;
  MakeFault(&fault, AddressInThisExecutable());
  EXPECT_FALSE(nevr_host::WriteCrashReport(&fault.pointers));
  EXPECT_TRUE(Reports().empty());
}
