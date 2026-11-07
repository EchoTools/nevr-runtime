#include "runtime/hook/script_hooks.h"

#include <winsock2.h>
#include <windows.h>

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <MinHook.h>

#include "core/logging.h"
#include "runtime/hook/patching.h"
#include "nevr/pc/nevr_common.h"

namespace nevr_script_hooks {
namespace {

struct Module {
  std::uint64_t script = 0;
  std::size_t refs = 0;
  std::vector<void*> targets;  // the hooks installed in this module, removed when the last reference goes
};

std::mutex g_mu;
std::vector<Hook> g_rows;
std::map<void*, Module> g_modules;

std::string Hex(const std::uint8_t* p, std::size_t n) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(n * 2);
  for (std::size_t i = 0; i < n; ++i) {
    out += kDigits[p[i] >> 4];
    out += kDigits[p[i] & 0xf];
  }
  return out;
}

// Whether [p, p+n) is committed memory this process can read, so a row whose RVA lies past the module's image is a
// refusal and not a fault.
bool Readable(const void* p, std::size_t n) {
  const auto* at = static_cast<const std::uint8_t*>(p);
  const auto* end = at + n;
  while (at < end) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(at, &mbi, sizeof mbi) != sizeof mbi || mbi.State != MEM_COMMIT) return false;
    const DWORD prot = mbi.Protect & 0xff;
    if ((mbi.Protect & PAGE_GUARD) != 0 || prot == PAGE_NOACCESS || prot == PAGE_EXECUTE) return false;
    at = static_cast<const std::uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
  }
  return true;
}

void Remove(const std::vector<void*>& targets, std::uint64_t script) {
  for (void* t : targets) {
    const MH_STATUS st = PatchRetire(t);
    if (st != MH_OK && st != MH_ERROR_NOT_CREATED) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.SCRIPT] hook not removed script=%016llx va=%p status=%s (it stays in place in a module the game is "
          "freeing, and is no longer guarded)",
          static_cast<unsigned long long>(script), t, MH_StatusToString(st));
    }
  }
}

}  // namespace

void Register(const Hook* rows, std::size_t count) {
  std::lock_guard<std::mutex> lock(g_mu);
  g_rows.insert(g_rows.end(), rows, rows + count);
}

void OnLoaded(std::uint64_t script, void* base) {
  if (base == nullptr) return;
  std::lock_guard<std::mutex> lock(g_mu);
  auto known = g_modules.find(base);
  if (known != g_modules.end()) {
    ++known->second.refs;
    Log(EchoVR::LogLevel::Debug, "[NEVR.SCRIPT] script loaded again script=%016llx base=%p refs=%zu hooks=%zu",
        static_cast<unsigned long long>(script), base, known->second.refs, known->second.targets.size());
    return;
  }
  Module m;
  m.script = script;
  m.refs = 1;

  std::vector<const Hook*> rows;
  for (const Hook& h : g_rows) {
    if (h.script == script) rows.push_back(&h);
  }
  if (rows.empty()) {
    g_modules.emplace(base, m);
    Log(EchoVR::LogLevel::Debug, "[NEVR.SCRIPT] script loaded script=%016llx base=%p hooks=0",
        static_cast<unsigned long long>(script), base);
    return;
  }

  // Every row is checked before any is installed: a script is hooked as written or runs stock.
  std::size_t mismatched = 0;
  for (const Hook* h : rows) {
    auto* target = static_cast<std::uint8_t*>(base) + h->rva;
    const bool readable = Readable(target, h->expected_len);
    if (readable && nevr::ValidatePrologue(target, h->expected, h->expected_len)) continue;
    ++mismatched;
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.SCRIPT] hook not installed script=%016llx name=\"%s\" rva=0x%x va=%p expected=%s actual=%s (this "
        "script's bytes are not the ones the hook was written against)",
        static_cast<unsigned long long>(script), h->name, h->rva, static_cast<void*>(target),
        Hex(h->expected, h->expected_len).c_str(), readable ? Hex(target, h->expected_len).c_str() : "unreadable");
  }
  if (mismatched > 0) {
    g_modules.emplace(base, m);
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.SCRIPT] script hooks refused script=%016llx base=%p rows=%zu mismatched=%zu (none of this script's hooks "
        "is installed; the script runs stock)",
        static_cast<unsigned long long>(script), base, rows.size(), mismatched);
    return;
  }

  for (const Hook* h : rows) {
    void* target = static_cast<std::uint8_t*>(base) + h->rva;
    *h->original = target;
    if (!PatchDetour(h->original, h->detour, h->name)) {
      // PatchDetour logged the cause. Take back what this load installed, so the script runs stock.
      Remove(m.targets, script);
      m.targets.clear();
      g_modules.emplace(base, m);
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.SCRIPT] script hooks refused script=%016llx base=%p rows=%zu failed=\"%s\" (the hooks this load had "
          "installed are removed; the script runs stock)",
          static_cast<unsigned long long>(script), base, rows.size(), h->name);
      return;
    }
    m.targets.push_back(target);
  }
  const std::size_t n = m.targets.size();
  g_modules.emplace(base, m);
  Log(EchoVR::LogLevel::Info, "[NEVR.SCRIPT] script hooks installed script=%016llx base=%p hooks=%zu",
      static_cast<unsigned long long>(script), base, n);
}

void OnUnloading(std::uint64_t script, void* base) {
  if (base == nullptr) return;
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_modules.find(base);
  if (it == g_modules.end()) {
    Log(EchoVR::LogLevel::Debug,
        "[NEVR.SCRIPT] script freed script=%016llx base=%p (loaded before the script hooks were in place)",
        static_cast<unsigned long long>(script), base);
    return;
  }
  if (--it->second.refs > 0) {
    Log(EchoVR::LogLevel::Debug, "[NEVR.SCRIPT] script reference freed script=%016llx base=%p refs=%zu",
        static_cast<unsigned long long>(script), base, it->second.refs);
    return;
  }
  const std::size_t n = it->second.targets.size();
  Remove(it->second.targets, it->second.script);
  g_modules.erase(it);
  if (n > 0) {
    Log(EchoVR::LogLevel::Info, "[NEVR.SCRIPT] script hooks removed script=%016llx base=%p hooks=%zu (the game frees it)",
        static_cast<unsigned long long>(script), base, n);
  } else {
    Log(EchoVR::LogLevel::Debug, "[NEVR.SCRIPT] script freed script=%016llx base=%p hooks=0",
        static_cast<unsigned long long>(script), base);
  }
}

std::size_t LoadedModules() {
  std::lock_guard<std::mutex> lock(g_mu);
  return g_modules.size();
}

std::size_t HookedModules() {
  std::lock_guard<std::mutex> lock(g_mu);
  std::size_t n = 0;
  for (const auto& [base, m] : g_modules) n += m.targets.empty() ? 0 : 1;
  return n;
}

void ResetForTest() {
  std::lock_guard<std::mutex> lock(g_mu);
  g_rows.clear();
  g_modules.clear();
}

}  // namespace nevr_script_hooks
