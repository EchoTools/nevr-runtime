#include "src/pnsrad/Core/CBaseErr.h"

namespace NRadEngine {

// @0x180097050 — GetGlobalMemoryContext
// @confidence: H
uint64_t GetGlobalMemoryContext() {
    // DAT_1803766b8 is a global pointer to the memory context
    static uint64_t* g_mem_context = nullptr;  // @0x1803766b8
    if (g_mem_context == nullptr) {
        LogAndAbort(8, 0, "Trying to use memory context before it's initialized!");
    }
    return *g_mem_context;
}

// @0x180097090 — CBasicErr::GetTypeHash
// @confidence: H
uint64_t* CBasicErr::GetTypeHash(uint64_t* out) const {
    *out = 0xb71e14605dc3f150ULL;
    return out;
}

// @0x1800970B0 — CErrMsg::GetTypeHash
// @confidence: H
uint64_t* CErrMsg::GetTypeHash(uint64_t* out) const {
    *out = 0x2fd48a8d4c79448dULL;
    return out;
}

// @0x1800970D0 — CBaseErr::FatalReport
// @confidence: H
void CBaseErr::FatalReport() const {
    LogAndAbort(8, 0, "%#016llx", error_code_);
}

// @0x1800970F0 — CBasicErr::FatalReport
// @confidence: H
void CBasicErr::FatalReport() const {
    // Additional source file/line args are passed via the base class data layout
    LogAndAbort(8, 0, "%#016llx in %s at line %d", error_code_);
}

// @0x180097130 — CErrMsg::FatalReport
// @confidence: H
void CErrMsg::FatalReport() const {
    LogAndAbort(8, 0, "%#016llx:\n    %s\n    ...in %s at line %d", error_code_);
}

} // namespace NRadEngine
