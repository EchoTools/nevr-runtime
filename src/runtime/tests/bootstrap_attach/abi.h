#pragma once

#include <windows.h>

#include <cstdint>

constexpr std::uint32_t kProbeAbiMajor = 1;

struct ProbeRuntimeApi {
  std::uint32_t struct_size;
  std::uint32_t abi_major;
  std::uint32_t abi_minor;
  BOOL(WINAPI* initialize)();
  void(WINAPI* preprocess_before)();
  void(WINAPI* preprocess_after)(std::uintptr_t result);
};

using ProbeGetRuntimeApi = const ProbeRuntimeApi*(WINAPI*)();
using ProbeTargetFunction = std::uintptr_t(WINAPI*)(std::uintptr_t, std::uintptr_t);

enum ProbeEvent : LONG {
  kHostAttachEnter = 1,
  kHostHookEnabled = 2,
  kHostAttachExit = 3,
  kHostLoadReturned = 4,
  kRuntimeInitialized = 5,
  kRuntimePreprocessBefore = 6,
  kTargetOriginal = 7,
  kRuntimePreprocessAfter = 8,
  kRuntimeFailure = 9,
};

extern "C" __declspec(dllexport) void WINAPI BootstrapProbeRecordEvent(LONG event);
