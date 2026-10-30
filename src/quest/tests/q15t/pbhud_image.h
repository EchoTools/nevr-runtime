// A fake game-library image for the powerboost / hudmeter / shieldgamma host tests: one lazily committed anonymous mapping
// so that `base + ELF vaddr` of the pinned build is a real, readable, writable address. Slot words (JUMP_SLOT / GLOB_DAT) are
// written at their real ELF vaddrs, the slot pages are made read-only as RELRO is, and a "real function" at an ELF vaddr is a
// 12-byte x86-64 trampoline (movabs rax, fn; jmp rax) on its own RWX page. x86-64 host only.
#pragma once

#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pbhud {

constexpr std::size_t kImageSize = 0x3730000;

struct Image {
  std::uint8_t* mem = nullptr;
  std::uintptr_t base = 0;

  Image() {
    void* p = mmap(nullptr, kImageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
      std::perror("mmap image");
      std::abort();
    }
    mem = static_cast<std::uint8_t*>(p);
    base = reinterpret_cast<std::uintptr_t>(p);
  }
  ~Image() { munmap(mem, kImageSize); }
  Image(const Image&) = delete;
  Image& operator=(const Image&) = delete;

  std::uintptr_t Abs(std::uintptr_t vaddr) const { return base + vaddr; }
  std::uintptr_t& Word(std::uintptr_t vaddr) { return *reinterpret_cast<std::uintptr_t*>(mem + vaddr); }
  std::uint32_t& Word32(std::uintptr_t vaddr) { return *reinterpret_cast<std::uint32_t*>(mem + vaddr); }

  // a real function of the game library at `vaddr`, implemented by `fn`
  void Real(std::uintptr_t vaddr, void* fn) {
    const std::uintptr_t page = (base + vaddr) & ~static_cast<std::uintptr_t>(4095);
    if (mprotect(reinterpret_cast<void*>(page), 4096, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
      std::perror("mprotect rwx");
      std::abort();
    }
    std::uint8_t* t = mem + vaddr;
    t[0] = 0x48;
    t[1] = 0xb8;
    const std::uint64_t a = reinterpret_cast<std::uint64_t>(fn);
    std::memcpy(t + 2, &a, 8);
    t[10] = 0xff;
    t[11] = 0xe0;
  }
  // a slot word at `slot` holding base + fn_vaddr, its page then read-only (RELRO)
  void Slot(std::uintptr_t slot, std::uintptr_t fn_vaddr) { SetSlot(slot, base + fn_vaddr); }
  // write one word of a read-only slot page (a test setup step, not the code under test)
  void SetSlot(std::uintptr_t slot, std::uintptr_t value) {
    const std::uintptr_t page = (base + slot) & ~static_cast<std::uintptr_t>(4095);
    mprotect(reinterpret_cast<void*>(page), 4096, PROT_READ | PROT_WRITE);
    Word(slot) = value;
    mprotect(reinterpret_cast<void*>(page), 4096, PROT_READ);
  }
  std::uintptr_t Get(std::uintptr_t slot) { return Word(slot); }
};

}  // namespace pbhud
