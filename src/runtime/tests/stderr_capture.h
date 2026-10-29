#pragma once

// A test's view of what the code under test writes to the process's standard error handle: the handle is pointed at a
// file for the life of the object and put back after. The host writes its NEVR.STOP line with one WriteFile to
// GetStdHandle(STD_ERROR_HANDLE) (runtime/host/stop_line.h), so a test reads the line from here.

#include <windows.h>

#include <cstdio>
#include <string>

namespace nevr_test {

class StderrCapture {
 public:
  StderrCapture() {
    wchar_t base[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, base);
    static LONG counter = 0;
    wchar_t name[MAX_PATH] = {};
    swprintf(name, MAX_PATH, L"%lsnevr-stderr-%lu-%llu-%ld.txt", base, static_cast<unsigned long>(GetCurrentProcessId()),
             static_cast<unsigned long long>(GetTickCount64()), static_cast<long>(InterlockedIncrement(&counter)));
    path_ = name;
    previous_ = GetStdHandle(STD_ERROR_HANDLE);
    file_ = CreateFileW(path_.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ != INVALID_HANDLE_VALUE) SetStdHandle(STD_ERROR_HANDLE, file_);
  }
  ~StderrCapture() {
    SetStdHandle(STD_ERROR_HANDLE, previous_);
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    DeleteFileW(path_.c_str());
  }
  StderrCapture(const StderrCapture&) = delete;
  StderrCapture& operator=(const StderrCapture&) = delete;

  /// Everything written so far.
  std::string Text() const {
    HANDLE in = CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (in == INVALID_HANDLE_VALUE) return {};
    std::string out;
    char buffer[512] = {};
    DWORD read = 0;
    while (ReadFile(in, buffer, sizeof(buffer), &read, nullptr) != FALSE && read != 0) out.append(buffer, read);
    CloseHandle(in);
    return out;
  }

  bool Capturing() const { return file_ != INVALID_HANDLE_VALUE; }

 private:
  std::wstring path_;
  HANDLE previous_ = nullptr;
  HANDLE file_ = INVALID_HANDLE_VALUE;
};

}  // namespace nevr_test
