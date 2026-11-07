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

struct Swapped {
  void** at;        // the slot in the script's data
  void* original;   // what the engine had put there
  const Slot* row;
};

struct Module {
  std::uint64_t script = 0;
  std::size_t refs = 0;
  std::vector<void*> targets;    // the hooks installed in this module, removed when the last reference goes
  std::vector<Swapped> swapped;  // the slots replaced in this module, put back before it is freed
};

std::mutex g_mu;
std::vector<Hook> g_rows;
std::vector<Slot> g_slots;
std::map<void*, Module> g_modules;

// Whether `p` lies in the game executable's image: where the engine callbacks a script calls through are.
bool InGameImage(const void* p) {
  const auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
  if (base == nullptr) return false;
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const auto* at = static_cast<const std::uint8_t*>(p);
  return at >= base && at < base + nt->OptionalHeader.SizeOfImage;
}

void PutBack(Module& m) {
  for (const Swapped& s : m.swapped) {
    // Only our own value is replaced: a slot the script or the engine rewrote since keeps what it holds, and says so.
    void* const found =
        InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(s.at), s.original, s.row->replacement);
    if (found != s.row->replacement) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.SCRIPT] slot not put back name=\"%s\" va=%p found=%p ours=%p (something rewrote the slot after this "
          "table replaced it; it keeps what it holds)",
          s.row->name, static_cast<void*>(s.at), found, s.row->replacement);
    }
    s.row->module->store(nullptr);
  }
  m.swapped.clear();
}

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

void Register(const Slot* slots, std::size_t count) {
  std::lock_guard<std::mutex> lock(g_mu);
  g_slots.insert(g_slots.end(), slots, slots + count);
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
  std::vector<const Slot*> slots;
  for (const Slot& s : g_slots) {
    if (s.script == script) slots.push_back(&s);
  }
  if (rows.empty() && slots.empty()) {
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
  for (const Slot* s : slots) {
    auto* at = reinterpret_cast<void**>(static_cast<std::uint8_t*>(base) + s->rva);
    const bool readable = Readable(at, sizeof(void*));
    void* value = readable ? *at : nullptr;
    if (readable && value != nullptr && InGameImage(value)) continue;
    ++mismatched;
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.SCRIPT] slot not replaced script=%016llx name=\"%s\" rva=0x%x va=%p value=%p (the slot does not hold an "
        "engine function from the game executable, so this script is not the one the hook was written against)",
        static_cast<unsigned long long>(script), s->name, s->rva, static_cast<void*>(at), readable ? value : nullptr);
  }
  if (mismatched > 0) {
    g_modules.emplace(base, m);
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.SCRIPT] script hooks refused script=%016llx base=%p rows=%zu mismatched=%zu (none of this script's hooks "
        "is installed; the script runs stock)",
        static_cast<unsigned long long>(script), base, rows.size() + slots.size(), mismatched);
    return;
  }

  std::size_t pins = 0;
  for (const Hook* h : rows) {
    if (h->detour == nullptr) {  // a pin: its bytes were checked above; nothing is installed there
      ++pins;
      continue;
    }
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
          static_cast<unsigned long long>(script), base, rows.size() + slots.size(), h->name);
      return;
    }
    m.targets.push_back(target);
  }
  for (const Slot* s : slots) {
    auto* at = reinterpret_cast<void**>(static_cast<std::uint8_t*>(base) + s->rva);
    void* engine = *at;
    *s->original = engine;
    s->module->store(base);
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(at), s->replacement);
    m.swapped.push_back(Swapped{at, engine, s});
  }
  const std::size_t n = m.targets.size();
  const std::size_t k = m.swapped.size();
  g_modules.emplace(base, m);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.SCRIPT] script hooks installed script=%016llx base=%p hooks=%zu slots=%zu pins=%zu",
      static_cast<unsigned long long>(script), base, n, k, pins);
}

void OnUnloading(std::uint64_t script, void* base) {
  if (base == nullptr) return;
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_modules.find(base);
  if (it == g_modules.end()) {
    Log(EchoVR::LogLevel::Debug,
        "[NEVR.SCRIPT] script freed script=%016llx base=%p (the table did not see it loaded: it loaded before the script hooks "
        "were in place, or its Load failed after the DLL loaded; it holds no hooks)",
        static_cast<unsigned long long>(script), base);
    return;
  }
  if (--it->second.refs > 0) {
    Log(EchoVR::LogLevel::Debug, "[NEVR.SCRIPT] script reference freed script=%016llx base=%p refs=%zu",
        static_cast<unsigned long long>(script), base, it->second.refs);
    return;
  }
  const std::size_t n = it->second.targets.size();
  const std::size_t k = it->second.swapped.size();
  PutBack(it->second);
  Remove(it->second.targets, it->second.script);
  g_modules.erase(it);
  if (n + k > 0) {
    Log(EchoVR::LogLevel::Info,
        "[NEVR.SCRIPT] script hooks removed script=%016llx base=%p hooks=%zu slots=%zu (the game frees it)",
        static_cast<unsigned long long>(script), base, n, k);
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
  for (const auto& [base, m] : g_modules) n += (m.targets.empty() && m.swapped.empty()) ? 0 : 1;
  return n;
}

void ResetForTest() {
  std::lock_guard<std::mutex> lock(g_mu);
  g_rows.clear();
  g_slots.clear();
  g_modules.clear();
}

}  // namespace nevr_script_hooks
