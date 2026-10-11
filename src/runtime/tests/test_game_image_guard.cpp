#include "runtime/lifecycle/game_image_guard.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>

namespace {

constexpr DWORD kExpectedTimestamp = 0x6452dff6;
constexpr DWORD kChildExitCode = nevr_game_image_guard::kUnsupportedImageExitCode;

class TestImage {
 public:
  TestImage() {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    page_size_ = info.dwPageSize;
    allocation_size_ = page_size_ * 3;
    base_ = static_cast<BYTE*>(VirtualAlloc(nullptr, allocation_size_,
                                             MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  }

  ~TestImage() {
    if (base_ != nullptr) VirtualFree(base_, 0, MEM_RELEASE);
  }

  TestImage(const TestImage&) = delete;
  TestImage& operator=(const TestImage&) = delete;

  BYTE* base() const { return base_; }
  SIZE_T page_size() const { return page_size_; }

  bool valid() const { return base_ != nullptr; }

  void PutDosHeader(BYTE* at, LONG pe_offset) {
    IMAGE_DOS_HEADER dos{};
    dos.e_magic = IMAGE_DOS_SIGNATURE;
    dos.e_lfanew = pe_offset;
    memcpy(at, &dos, sizeof(dos));
  }

  void PutNtHeaders(BYTE* at, DWORD timestamp = kExpectedTimestamp) {
    IMAGE_NT_HEADERS64 nt{};
    nt.Signature = IMAGE_NT_SIGNATURE;
    nt.FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt.FileHeader.NumberOfSections = 1;
    nt.FileHeader.TimeDateStamp = timestamp;
    nt.FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt.OptionalHeader.SizeOfHeaders = 0x1000;
    nt.OptionalHeader.SizeOfImage = 0x3000;
    memcpy(at, &nt, sizeof(nt));
  }

  void MakeValid(BYTE* at = nullptr) {
    if (at == nullptr) at = base_;
    PutDosHeader(at, static_cast<LONG>(sizeof(IMAGE_DOS_HEADER)));
    PutNtHeaders(at + sizeof(IMAGE_DOS_HEADER));
  }

 private:
  BYTE* base_ = nullptr;
  SIZE_T page_size_ = 0;
  SIZE_T allocation_size_ = 0;
};

DWORD RunChild(const std::string& mode, HANDLE stderr_handle = INVALID_HANDLE_VALUE,
               DWORD wait_ms = 5000) {
  std::array<char, MAX_PATH> exe{};
  const DWORD length = GetModuleFileNameA(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
  if (length == 0 || length >= exe.size()) return MAXDWORD;

  std::string command = "\"" + std::string(exe.data(), length) + "\" " + mode;
  std::vector<char> writable(command.begin(), command.end());
  writable.push_back('\0');

  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  if (stderr_handle != INVALID_HANDLE_VALUE) {
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = stderr_handle;
    startup.hStdError = stderr_handle;
  }
  PROCESS_INFORMATION process{};
  const BOOL created = CreateProcessA(nullptr, writable.data(), nullptr, nullptr,
                                      stderr_handle != INVALID_HANDLE_VALUE,
                                      CREATE_NO_WINDOW, nullptr, nullptr,
                                      &startup, &process);
  if (!created) return MAXDWORD;
  CloseHandle(process.hThread);
  const DWORD waited = WaitForSingleObject(process.hProcess, wait_ms);
  if (waited != WAIT_OBJECT_0) {
    TerminateProcess(process.hProcess, MAXDWORD);
    WaitForSingleObject(process.hProcess, INFINITE);
    CloseHandle(process.hProcess);
    return MAXDWORD - 1;
  }
  DWORD exit_code = MAXDWORD;
  GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hProcess);
  return exit_code;
}

void InitializeChildModule(HMODULE) {}

struct PipeWriterContext {
  HANDLE write_pipe = nullptr;
  HANDLE ready_event = nullptr;
};

DWORD WINAPI FillPipeUntilBlocked(void* raw_context) {
  auto* context = static_cast<PipeWriterContext*>(raw_context);
  std::array<char, 4096> bytes{};
  SetEvent(context->ready_event);
  for (;;) {
    DWORD written = 0;
    if (!WriteFile(context->write_pipe, bytes.data(), static_cast<DWORD>(bytes.size()),
                   &written, nullptr)) {
      return 0;
    }
  }
}

struct PipeResources {
  HANDLE read_pipe = nullptr;
  HANDLE write_pipe = nullptr;
  HANDLE ready_event = nullptr;
  HANDLE writer_thread = nullptr;

  ~PipeResources() {
    if (read_pipe != nullptr) CloseHandle(read_pipe);
    if (writer_thread != nullptr) {
      WaitForSingleObject(writer_thread, 3000);
      CloseHandle(writer_thread);
    }
    if (ready_event != nullptr) CloseHandle(ready_event);
    if (write_pipe != nullptr) CloseHandle(write_pipe);
  }
};

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && (strcmp(argv[1], "--child-dllmain-load") == 0 ||
                    strcmp(argv[1], "--child-launcher-load") == 0 ||
                    strcmp(argv[1], "--child-full-pipe") == 0)) {
    nevr_game_image_guard::RunWithSupportedGameModule(
        reinterpret_cast<HMODULE>(uintptr_t{1}), &InitializeChildModule);
    return 0;
  }
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

TEST(GameImageGuard, AcceptsOnlySupportedTimestampFromReadablePe32PlusHeaders) {
  TestImage image;
  ASSERT_TRUE(image.valid());
  image.MakeValid();
  EXPECT_TRUE(nevr_game_image_guard::IsSupportedGameModule(image.base()));
}

TEST(GameImageGuard, RejectsNullBadDosAndInvalidOffsets) {
  EXPECT_FALSE(nevr_game_image_guard::IsSupportedGameModule(nullptr));
  TestImage image;
  ASSERT_TRUE(image.valid());
  image.MakeValid();
  image.base()[0] = 0;
  EXPECT_FALSE(nevr_game_image_guard::IsSupportedGameModule(image.base()));
  image.MakeValid();
  image.PutDosHeader(image.base(), -1);
  EXPECT_FALSE(nevr_game_image_guard::IsSupportedGameModule(image.base()));
  image.PutDosHeader(image.base(), 0x100001);
  EXPECT_FALSE(nevr_game_image_guard::IsSupportedGameModule(image.base()));
}

TEST(GameImageGuard, RejectsUnexpectedTimestampAndTruncatedOptionalHeader) {
  TestImage image;
  ASSERT_TRUE(image.valid());
  image.PutDosHeader(image.base(), sizeof(IMAGE_DOS_HEADER));
  image.PutNtHeaders(image.base() + sizeof(IMAGE_DOS_HEADER), kExpectedTimestamp + 1);
  EXPECT_FALSE(nevr_game_image_guard::IsSupportedGameModule(image.base()));
  image.PutNtHeaders(image.base() + sizeof(IMAGE_DOS_HEADER));
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(image.base() + sizeof(IMAGE_DOS_HEADER));
  nt->FileHeader.SizeOfOptionalHeader = sizeof(WORD);
  EXPECT_FALSE(nevr_game_image_guard::IsSupportedGameModule(image.base()));
}

TEST(GameImageGuard, RejectsPeHeaderSplitAcrossUncommittedPageBoundary) {
  TestImage image;
  ASSERT_TRUE(image.valid());
  BYTE* second_page = image.base() + image.page_size();
  ASSERT_NE(VirtualFree(second_page, image.page_size(), MEM_DECOMMIT), FALSE);
  const LONG pe_offset = static_cast<LONG>(image.page_size() - sizeof(WORD) - sizeof(IMAGE_DOS_HEADER));
  image.PutDosHeader(image.base(), pe_offset);
  EXPECT_FALSE(nevr_game_image_guard::IsSupportedGameModule(image.base()));
}

TEST(GameImageGuard, RejectsGuardAndNoAccessHeaderPages) {
  TestImage image;
  ASSERT_TRUE(image.valid());
  const LONG pe_offset = static_cast<LONG>(image.page_size() - 8 - sizeof(IMAGE_DOS_HEADER));
  image.PutDosHeader(image.base(), pe_offset);
  DWORD prior = 0;
  ASSERT_NE(VirtualProtect(image.base() + image.page_size(), image.page_size(), PAGE_NOACCESS, &prior), FALSE);
  EXPECT_FALSE(nevr_game_image_guard::IsSupportedGameModule(image.base()));
  ASSERT_NE(VirtualProtect(image.base() + image.page_size(), image.page_size(), PAGE_READWRITE, &prior), FALSE);
  ASSERT_NE(VirtualProtect(image.base() + image.page_size(), image.page_size(), PAGE_READWRITE | PAGE_GUARD, &prior), FALSE);
  EXPECT_FALSE(nevr_game_image_guard::IsSupportedGameModule(image.base()));
}

TEST(GameImageGuard, RejectsModulePointerThatIsNotAllocationBase) {
  TestImage image;
  ASSERT_TRUE(image.valid());
  BYTE* suballocation = image.base() + image.page_size();
  image.MakeValid(suballocation);
  EXPECT_FALSE(nevr_game_image_guard::IsSupportedGameModule(suballocation));
}

TEST(GameImageGuard, RejectsAddressArithmeticOverflowBeforeReading) {
  const uintptr_t near_limit = UINTPTR_MAX - 16;
  EXPECT_FALSE(nevr_game_image_guard::IsSupportedGameModule(reinterpret_cast<HMODULE>(near_limit)));
}

TEST(GameImageGuard, DllMainLoadAndLauncherChildrenTerminateWithExactCode) {
  EXPECT_EQ(RunChild("--child-dllmain-load"), kChildExitCode);
  EXPECT_EQ(RunChild("--child-launcher-load"), kChildExitCode);
}

TEST(GameImageGuard, RejectionDoesNotBlockOnFullInheritedStderrPipe) {
  PipeResources pipes;
  SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
  EXPECT_NE(CreatePipe(&pipes.read_pipe, &pipes.write_pipe, &security, 4096), FALSE);
  if (pipes.read_pipe == nullptr || pipes.write_pipe == nullptr) return;
  EXPECT_NE(SetHandleInformation(pipes.read_pipe, HANDLE_FLAG_INHERIT, 0), FALSE);
  pipes.ready_event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  EXPECT_NE(pipes.ready_event, nullptr);
  if (pipes.ready_event == nullptr) return;
  PipeWriterContext writer_context{pipes.write_pipe, pipes.ready_event};
  pipes.writer_thread = CreateThread(nullptr, 0, &FillPipeUntilBlocked, &writer_context, 0, nullptr);
  EXPECT_NE(pipes.writer_thread, nullptr);
  if (pipes.writer_thread == nullptr) return;
  EXPECT_EQ(WaitForSingleObject(pipes.ready_event, 1000), WAIT_OBJECT_0);
  DWORD available = 0;
  bool observed_data = false;
  for (DWORD attempt = 0; attempt < 100; ++attempt) {
    if (PeekNamedPipe(pipes.read_pipe, nullptr, 0, nullptr, &available, nullptr) == FALSE) break;
    if (available > 0) {
      observed_data = true;
      break;
    }
    Sleep(10);
  }
  EXPECT_TRUE(observed_data);
  if (!observed_data) return;
  const DWORD writer_status = WaitForSingleObject(pipes.writer_thread, 250);
  EXPECT_EQ(writer_status, WAIT_TIMEOUT);
  if (writer_status != WAIT_TIMEOUT) return;

  EXPECT_EQ(RunChild("--child-full-pipe", pipes.write_pipe, 3000), kChildExitCode);
}
