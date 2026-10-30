#include "import_hook.h"

#include <dlfcn.h>

#include <cstring>

namespace q15t {
namespace {

std::uintptr_t ResolveRuntimeSymbol(const char* name) {
  void* p = dlsym(RTLD_DEFAULT, name);
  if (p == nullptr) {
    void* h = dlopen("libvrapi.so", RTLD_NOW | RTLD_NOLOAD);
    if (h != nullptr) p = dlsym(h, name);
  }
  return reinterpret_cast<std::uintptr_t>(p);
}

}  // namespace

InstallResult InstallImportHook(const char* symbol, std::uintptr_t base, std::uintptr_t slot_vaddr, std::uintptr_t wrapper,
                                std::atomic<std::uintptr_t>* real_out, std::uintptr_t expected_vaddr) {
  InstallResult out;
  if (base == 0) {
    out.reason = "no_base";
    return out;
  }
  const std::uintptr_t slot = base + slot_vaddr;
  std::uintptr_t expected = expected_vaddr != 0 ? base + expected_vaddr : ResolveRuntimeSymbol(symbol);
  out.real_resolved = expected != 0;
  if (expected == 0) {
    // The runtime's symbol was not resolvable: accept the word only if it points into an executable file mapping that is
    // not the game's own library (an unresolved lazy slot points back into the game's .plt).
    Mapping m;
    if (slot % sizeof(std::uintptr_t) != 0 || !FindMapping(slot, &m)) {
      out.status = SwapStatus::kUnmapped;
      return out;
    }
    const std::uintptr_t word = *reinterpret_cast<const std::uintptr_t*>(slot);
    Mapping target;
    if (!FindMapping(word, &target) || target.perms[2] != 'x' || target.path[0] == 0 ||
        std::strstr(target.path, "libr15") != nullptr) {
      out.status = SwapStatus::kUnexpectedOriginal;
      out.reason = "word_not_in_runtime_text";
      return out;
    }
    expected = word;
  }
  real_out->store(expected, std::memory_order_release);
  const SwapResult r = SwapWord(slot, expected, wrapper);
  out.status = r.status;
  if (r.status != SwapStatus::kOk && r.status != SwapStatus::kRestoreFailed) real_out->store(0, std::memory_order_release);
  return out;
}

InstallResult InstallChainedImportHook(std::uintptr_t base, std::uintptr_t slot_vaddr, std::uintptr_t wrapper,
                                       std::atomic<std::uintptr_t>* real_out, std::uintptr_t stock_vaddr) {
  InstallResult out;
  if (base == 0 || stock_vaddr == 0) {
    out.reason = "no_base";
    return out;
  }
  const std::uintptr_t slot = base + slot_vaddr;
  Mapping m;
  if (slot % sizeof(std::uintptr_t) != 0 || !FindMapping(slot, &m)) {
    out.status = SwapStatus::kUnmapped;
    return out;
  }
  const std::uintptr_t word = *reinterpret_cast<const std::uintptr_t*>(slot);
  if (word == wrapper) {  // this wrapper is already in the slot: chaining on it would call itself
    out.status = SwapStatus::kUnexpectedOriginal;
    out.reason = "already_installed";
    return out;
  }
  bool accepted = word == base + stock_vaddr;
  if (!accepted && word != 0) {
    Mapping own, target;
    accepted = FindMapping(reinterpret_cast<std::uintptr_t>(&InstallChainedImportHook), &own) && FindMapping(word, &target) &&
               target.perms[2] == 'x' && target.path[0] != 0 && std::strcmp(target.path, own.path) == 0;
  }
  if (!accepted) {
    out.status = SwapStatus::kUnexpectedOriginal;
    out.reason = "slot_not_stock_or_own";
    return out;
  }
  out.real_resolved = true;
  real_out->store(word, std::memory_order_release);
  const SwapResult r = SwapWord(slot, word, wrapper);
  out.status = r.status;
  if (r.status != SwapStatus::kOk && r.status != SwapStatus::kRestoreFailed) real_out->store(0, std::memory_order_release);
  return out;
}

}  // namespace q15t
