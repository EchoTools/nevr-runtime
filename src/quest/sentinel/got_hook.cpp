#include "got_hook.h"

#include <link.h>   // dl_iterate_phdr, ElfW, struct dl_phdr_info
#include <elf.h>    // Elf64_Dyn/Sym/Rela, DT_*, PT_DYNAMIC, ELF64_R_SYM/TYPE

#include <cstdint>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace sentinel {

namespace {

// Does `path` (possibly a full install path) end in `soName`?
bool NameMatches(const char* path, const char* soName) {
    if (!path) return false;
    const size_t pathLen = strlen(path);
    const size_t nameLen = strlen(soName);
    if (nameLen > pathLen) return false;
    const char* tail = path + (pathLen - nameLen);
    if (strcmp(tail, soName) != 0) return false;
    // Must be a full path-segment match, not a substring of a longer filename
    // (e.g. "libpnsrad.so" must not match "libpnsradmatchmaking.so").
    return pathLen == nameLen || path[pathLen - nameLen - 1] == '/';
}

struct FindResult {
    const char*    wantedSoName;
    ElfW(Addr)     base;
    const ElfW(Phdr)* phdr;
    ElfW(Half)     phnum;
    bool           found;
};

int FindModuleCallback(struct dl_phdr_info* info, size_t /*size*/, void* data) {
    auto* r = static_cast<FindResult*>(data);
    if (!NameMatches(info->dlpi_name, r->wantedSoName)) return 0;
    r->base  = info->dlpi_addr;
    r->phdr  = info->dlpi_phdr;
    r->phnum = info->dlpi_phnum;
    r->found = true;
    return 1;  // stop iterating
}

// Scan one DT_RELA-style relocation table for a JUMP_SLOT/GLOB_DAT/ABS64
// relocation naming `symbolName`. Returns the runtime GOT-slot address
// (base already applied) or nullptr if not present in this table.
void* FindGotSlot(ElfW(Addr) base, const Elf64_Rela* relaTable, size_t relaCount,
                  const Elf64_Sym* symtab, const char* strtab, const char* symbolName) {
    for (size_t i = 0; i < relaCount; i++) {
        const Elf64_Rela& rel = relaTable[i];
        const uint32_t type = ELF64_R_TYPE(rel.r_info);
        if (type != R_AARCH64_JUMP_SLOT && type != R_AARCH64_GLOB_DAT &&
            type != R_AARCH64_ABS64) {
            continue;
        }
        const uint32_t symIdx = ELF64_R_SYM(rel.r_info);
        const char* name = strtab + symtab[symIdx].st_name;
        if (strcmp(name, symbolName) == 0) {
            return reinterpret_cast<void*>(base + rel.r_offset);
        }
    }
    return nullptr;
}

}  // namespace

bool HookImport(const char* moduleSoName, const char* symbolName, void* hookFn,
                void** originalOut) {
    FindResult find{moduleSoName, 0, nullptr, 0, false};
    dl_iterate_phdr(FindModuleCallback, &find);
    if (!find.found) return false;  // module not currently loaded — not an error

    // Locate PT_DYNAMIC and walk it. All pointer-valued DT_* entries are
    // link-time (p_vaddr-relative) addresses for an ET_DYN .so and must be
    // adjusted by the load bias (find.base) to become runtime addresses.
    const Elf64_Dyn* dyn = nullptr;
    for (ElfW(Half) i = 0; i < find.phnum; i++) {
        if (find.phdr[i].p_type == PT_DYNAMIC) {
            dyn = reinterpret_cast<const Elf64_Dyn*>(find.base + find.phdr[i].p_vaddr);
            break;
        }
    }
    if (!dyn) return false;  // no PT_DYNAMIC — not a valid dynamic ELF, bail quietly

    const char*       strtab   = nullptr;
    const Elf64_Sym*  symtab   = nullptr;
    const Elf64_Rela* jmprel   = nullptr;
    size_t            jmprelsz = 0;
    const Elf64_Rela* rela     = nullptr;
    size_t            relasz   = 0;

    for (const Elf64_Dyn* d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_STRTAB: strtab = reinterpret_cast<const char*>(find.base + d->d_un.d_ptr); break;
            case DT_SYMTAB: symtab = reinterpret_cast<const Elf64_Sym*>(find.base + d->d_un.d_ptr); break;
            case DT_JMPREL: jmprel = reinterpret_cast<const Elf64_Rela*>(find.base + d->d_un.d_ptr); break;
            case DT_PLTRELSZ: jmprelsz = d->d_un.d_val; break;
            case DT_RELA: rela = reinterpret_cast<const Elf64_Rela*>(find.base + d->d_un.d_ptr); break;
            case DT_RELASZ: relasz = d->d_un.d_val; break;
            default: break;
        }
    }
    if (!strtab || !symtab) return false;  // malformed/stripped beyond what we need

    void* slot = nullptr;
    if (jmprel && jmprelsz) {
        slot = FindGotSlot(find.base, jmprel, jmprelsz / sizeof(Elf64_Rela), symtab, strtab,
                           symbolName);
    }
    if (!slot && rela && relasz) {
        slot = FindGotSlot(find.base, rela, relasz / sizeof(Elf64_Rela), symtab, strtab,
                           symbolName);
    }
    if (!slot) return false;  // this module doesn't call that symbol — lookup miss, not an error

    // mprotect the containing page(s) RW, swap the pointer, drop back to RO.
    // .got.plt is ordinary data (never executable) both before and after this.
    const long pageSize = sysconf(_SC_PAGESIZE);
    const uintptr_t addr = reinterpret_cast<uintptr_t>(slot);
    const uintptr_t pageStart = addr & ~(static_cast<uintptr_t>(pageSize) - 1);
    const uintptr_t pageEnd =
        (addr + sizeof(void*) + pageSize - 1) & ~(static_cast<uintptr_t>(pageSize) - 1);
    const size_t regionLen = pageEnd - pageStart;

    if (mprotect(reinterpret_cast<void*>(pageStart), regionLen, PROT_READ | PROT_WRITE) != 0) {
        return false;  // e.g. RELRO segment the kernel refuses to reprotect — bail, don't fault
    }

    void** gotSlot = reinterpret_cast<void**>(slot);
    if (originalOut) *originalOut = *gotSlot;
    *gotSlot = hookFn;

    mprotect(reinterpret_cast<void*>(pageStart), regionLen, PROT_READ);
    return true;
}

}  // namespace sentinel
