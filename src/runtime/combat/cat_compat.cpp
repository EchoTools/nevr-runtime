#include "runtime/combat/cat_compat.h"

#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "core/logging.h"
#include "runtime/combat/balance_values.h"
#include "runtime/combat/shipped_settings.h"
#include "runtime/combat/write_gate.h"
#include "runtime/hook/patching.h"
#include "runtime/lifecycle/service_config.h"

namespace nevr_combat::cat {

// --- __try / __except --------------------------------------------------------------------------------------------

namespace {

thread_local SehFrame* t_top = nullptr;
thread_local DWORD t_lastCode = 0;
PVOID g_handler = nullptr;
SRWLOCK g_handlerLock = SRWLOCK_INIT;

// The faults a __try block of Cat's catches: reading or writing memory that is not there, and the arithmetic and
// instruction faults an engine read through a stale pointer can raise. Not a debugger trap (crash_recovery.cpp owns
// those, N82), not a C++ exception, not a stack overflow (no stack to jump with).
bool Caught(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
      return true;
    default:
      return false;
  }
}

// Runs first in the vectored chain. With no __try block of Cat's open on this thread it passes every exception on.
LONG CALLBACK FaultHandler(EXCEPTION_POINTERS* info) {
  SehFrame* frame = t_top;
  if (frame == nullptr || info == nullptr || info->ExceptionRecord == nullptr) return EXCEPTION_CONTINUE_SEARCH;
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  if (!Caught(code)) return EXCEPTION_CONTINUE_SEARCH;
  t_lastCode = code;
  t_top = frame->prev;
  frame->armed = false;
  longjmp(frame->buf, 1);
}

}  // namespace

bool SehFrame::Enter(int jumped) {
  if (jumped != 0) return false;
  prev = t_top;
  t_top = this;
  armed = true;
  return true;
}

SehFrame::~SehFrame() {
  if (armed && t_top == this) t_top = prev;
  armed = false;
}

DWORD LastExceptionCode() { return t_lastCode; }

void InstallFaultHandler() {
  AcquireSRWLockExclusive(&g_handlerLock);
  if (g_handler == nullptr) {
    g_handler = AddVectoredExceptionHandler(1, &FaultHandler);
    if (g_handler == nullptr) {
      Log(EchoVR::LogLevel::Error,
          "[NEVR.COMBAT] fault handler not installed error=%lu (a fault inside EchoCombat's guarded code is then not "
          "contained)",
          GetLastError());
    }
  }
  ReleaseSRWLockExclusive(&g_handlerLock);
}

// --- Detours -----------------------------------------------------------------------------------------------------

namespace {

struct Hook {
  PVOID* real;     // the caller's pointer: the trampoline while hooked
  PVOID target;    // the game's function
  PVOID detour;
  const char* name;
};

constexpr int kMaxHooks = 96;
Hook g_hooks[kMaxHooks];
int g_hookCount = 0;
SRWLOCK g_hookLock = SRWLOCK_INIT;

// The current transaction: the hooks it made, and its first failure.
constexpr int kMaxPending = 32;
thread_local int t_pending[kMaxPending];
thread_local int t_pendingCount = 0;
thread_local LONG t_failure = NO_ERROR;
thread_local bool t_open = false;

std::uintptr_t Rva(const void* va) {
  const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
  const auto v = reinterpret_cast<std::uintptr_t>(va);
  return v >= base ? v - base : 0;
}

void RemoveAt(int i) {
  Hook& h = g_hooks[i];
  const MH_STATUS status = PatchRetire(h.target);
  if (status != MH_OK && status != MH_ERROR_NOT_CREATED) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.COMBAT] hook removal failed name=%s va=%p status=%s", h.name, h.target,
        MH_StatusToString(status));
  }
  *h.real = h.target;
  g_hooks[i] = g_hooks[--g_hookCount];
}

}  // namespace

LONG TransactionBegin() {
  t_open = true;
  t_pendingCount = 0;
  t_failure = NO_ERROR;
  return NO_ERROR;
}

LONG Attach(PVOID* real, PVOID detour, const char* name, const char* module) {
  if (real == nullptr || *real == nullptr || detour == nullptr) {
    if (t_failure == NO_ERROR) t_failure = ERROR_INVALID_PARAMETER;
    return ERROR_INVALID_PARAMETER;
  }
  PVOID target = *real;
  // The detour writes the target's first bytes: the 16 HookGuard compares cover MinHook's 5- and 14-byte jumps.
  if (!nevr_combat::GateWrite(target, 16, name)) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.COMBAT] hook refused module=%s name=%s va=%p rva=0x%llX reason=overlap", module,
        name, target, static_cast<unsigned long long>(Rva(target)));
    if (t_failure == NO_ERROR) t_failure = ERROR_ACCESS_DENIED;
    return ERROR_ACCESS_DENIED;
  }
  AcquireSRWLockExclusive(&g_hookLock);
  const bool room = g_hookCount < kMaxHooks && (!t_open || t_pendingCount < kMaxPending);
  ReleaseSRWLockExclusive(&g_hookLock);
  if (!room) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.COMBAT] hook refused module=%s name=%s va=%p reason=hook_table_full", module, name,
        target);
    if (t_failure == NO_ERROR) t_failure = ERROR_NOT_ENOUGH_MEMORY;
    return ERROR_NOT_ENOUGH_MEMORY;
  }
  if (!PatchDetour(real, detour, name)) {  // PatchDetour logs the failed stage at Warning (N126)
    Log(EchoVR::LogLevel::Warning, "[NEVR.COMBAT] hook refused module=%s name=%s va=%p rva=0x%llX reason=%s", module, name,
        target, static_cast<unsigned long long>(Rva(target)), Hooking::LastAttachError());
    if (t_failure == NO_ERROR) t_failure = ERROR_INVALID_OPERATION;
    return ERROR_INVALID_OPERATION;
  }
  AcquireSRWLockExclusive(&g_hookLock);
  const int index = g_hookCount++;
  g_hooks[index] = Hook{real, target, detour, name};
  if (t_open) t_pending[t_pendingCount++] = index;
  ReleaseSRWLockExclusive(&g_hookLock);
  Log(EchoVR::LogLevel::Info, "[NEVR.COMBAT] hook applied module=%s name=%s va=%p rva=0x%llX", module, name, target,
      static_cast<unsigned long long>(Rva(target)));
  return NO_ERROR;
}

LONG TransactionAbort() {
  // Detours applies nothing until the commit, so an abort or a failed commit leaves no hook: the ones this transaction
  // already made are taken out again, newest first.
  AcquireSRWLockExclusive(&g_hookLock);
  for (int k = t_pendingCount - 1; k >= 0; --k) {
    const int i = t_pending[k];
    if (i < g_hookCount) {
      Log(EchoVR::LogLevel::Warning, "[NEVR.COMBAT] hook rolled back name=%s va=%p (its transaction failed)", g_hooks[i].name,
          g_hooks[i].target);
      RemoveAt(i);
    }
  }
  ReleaseSRWLockExclusive(&g_hookLock);
  t_pendingCount = 0;
  t_open = false;
  return NO_ERROR;
}

LONG TransactionCommit() {
  const LONG failure = t_failure;
  if (failure != NO_ERROR) TransactionAbort();
  t_pendingCount = 0;
  t_open = false;
  t_failure = NO_ERROR;
  return failure;
}

LONG Detach(PVOID* real, PVOID detour) {
  AcquireSRWLockExclusive(&g_hookLock);
  for (int i = 0; i < g_hookCount; ++i) {
    if (g_hooks[i].real == real && g_hooks[i].detour == detour) {
      const char* name = g_hooks[i].name;
      PVOID target = g_hooks[i].target;
      RemoveAt(i);
      ReleaseSRWLockExclusive(&g_hookLock);
      Log(EchoVR::LogLevel::Info, "[NEVR.COMBAT] hook removed name=%s va=%p", name, target);
      return NO_ERROR;
    }
  }
  ReleaseSRWLockExclusive(&g_hookLock);
  return ERROR_INVALID_HANDLE;
}

// --- the plugin logger -------------------------------------------------------------------------------------------

void HostLog(const char* module, std::uint32_t level, const char* text) {
  EchoVR::LogLevel l = EchoVR::LogLevel::Info;
  switch (level) {
    case NEVR_LOG_DEBUG: l = EchoVR::LogLevel::Debug; break;
    case NEVR_LOG_WARNING: l = EchoVR::LogLevel::Warning; break;
    case NEVR_LOG_ERROR: l = EchoVR::LogLevel::Error; break;
    default: break;
  }
  Log(l, "[NEVR.COMBAT] module=%s %s", module != nullptr ? module : "?", text != nullptr ? text : "");
}

// --- PowerBoost.ini -> config.yaml -------------------------------------------------------------------------------

namespace {
// #600: a key config.yaml leaves out takes the value his release writes into PowerBoost.ini from his settings
// (shipped_settings.h), and the caller's literal only for a key his PowerBoost.ini does not carry. A shipped value is
// decided once and kept, so the pointer a caller gets stays valid. Each key's source is one Debug line, the first time the
// key is read (MEDIGUN re-reads its offsets once a second).
std::mutex g_iniLock;
std::map<std::string, std::string> g_shipped;  // "section.key" -> his value
std::map<std::string, bool> g_sourceLogged;    // "section.key" -> its source line was written

void LogSourceOnce(const std::string& name, const char* source, const char* value) {
  std::lock_guard<std::mutex> lock(g_iniLock);
  if (!g_sourceLogged.emplace(name, true).second) return;
  Log(EchoVR::LogLevel::Debug, "[NEVR.COMBAT] setting %s=%s source=%s", name.c_str(), value != nullptr ? value : "(code)", source);
}

const char* IniValue(const char* section, const char* key) {
  if (section == nullptr || key == nullptr) return nullptr;
  const std::string name = std::string(section) + "." + key;
  if (const char* v = NevrCfgCombatValue(("settings." + name).c_str())) {
    LogSourceOnce(name, "config.yaml", v);
    return v;
  }
  const char* shipped_value = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_iniLock);
    auto it = g_shipped.find(name);
    if (it == g_shipped.end()) {
      static const nlohmann::json settings = nlohmann::json::parse(nevr_combat::balance::kCatWeaponSettings, nullptr, false);
      if (const std::optional<std::string> shipped = nevr_combat::ShippedSetting(settings, section, key)) {
        it = g_shipped.emplace(name, *shipped).first;
      }
    }
    if (it != g_shipped.end()) shipped_value = it->second.c_str();
  }
  LogSourceOnce(name, shipped_value != nullptr ? "shipped" : "code (not in his PowerBoost.ini)", shipped_value);
  return shipped_value;
}
}  // namespace

int IniInt(const char* section, const char* key, int fallback) {
  const char* v = IniValue(section, key);
  if (v == nullptr || *v == '\0') return fallback;
  char* end = nullptr;
  const long n = std::strtol(v, &end, 10);  // GetPrivateProfileInt reads the leading digits, as strtol does
  return end == v ? fallback : static_cast<int>(n);
}

DWORD IniString(const char* section, const char* key, const char* fallback, char* out, DWORD size) {
  if (out == nullptr || size == 0) return 0;
  const char* v = IniValue(section, key);
  const char* text = v != nullptr ? v : (fallback != nullptr ? fallback : "");
  const int n = std::snprintf(out, size, "%s", text);
  return n < 0 ? 0 : static_cast<DWORD>(static_cast<DWORD>(n) < size ? n : size - 1);
}

}  // namespace nevr_combat::cat
