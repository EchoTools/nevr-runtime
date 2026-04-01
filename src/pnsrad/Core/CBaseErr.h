#ifndef PNSRAD_CORE_CBASEERR_H
#define PNSRAD_CORE_CBASEERR_H

/* @module: pnsrad.dll */
/* @purpose: Error hierarchy — CBaseErr / CBasicErr / CErrMsg */
/* @source: d:\projects\rad\dev\src\engine\libs\... */

#include <cstdint>

namespace NRadEngine {

// Forward declaration for the fatal error function
// @0x1800929a0 — LogAndAbort(level, ctx, fmt, ...)
extern void LogAndAbort(int level, int ctx, const char* fmt, ...);

// @0x180097050 — returns *DAT_1803766b8 (global memory context singleton)
// @confidence: H
extern uint64_t GetGlobalMemoryContext();

// ============================================================================
// CBaseErr — base error class with 64-bit error code
// ============================================================================

/* @addr: vtable inferred from vfunction3/vfunction5 entries */
/* @confidence: H */
class CBaseErr {
public:
    // @0x1800970D0 — CBaseErr::vfunction5
    // Calls LogAndAbort with "%#016llx" format printing error_code_
    virtual void FatalReport() const;

    // Virtual destructor implied by vtable
    virtual ~CBaseErr() = default;

    // @0x180097090 / 0x1800970B0 — vfunction3 returns a 64-bit hash identifying the error type
    virtual uint64_t* GetTypeHash(uint64_t* out) const = 0;

    uint64_t error_code_;  // +0x08
};

// ============================================================================
// CBasicErr — error with source file/line info
// ============================================================================

/* @addr: vtable inferred from vfunction3/vfunction5 entries */
/* @confidence: H */
class CBasicErr : public CBaseErr {
public:
    // @0x180097090 — CBasicErr::vfunction3
    // Returns 0xb71e14605dc3f150 as type hash
    uint64_t* GetTypeHash(uint64_t* out) const override;

    // @0x1800970F0 — CBasicErr::vfunction5
    // Calls LogAndAbort with "%#016llx in %s at line %d"
    void FatalReport() const override;

    // Source location fields set by the constructor
    // (file and line accessed via format args in FatalReport)
};

// ============================================================================
// CErrMsg — error with additional message string
// ============================================================================

/* @addr: vtable inferred from vfunction3/vfunction5 entries */
/* @confidence: H */
class CErrMsg : public CBaseErr {
public:
    // @0x1800970B0 — CErrMsg::vfunction3
    // Returns 0x2fd48a8d4c79448d as type hash
    uint64_t* GetTypeHash(uint64_t* out) const override;

    // @0x180097130 — CErrMsg::vfunction5
    // Calls LogAndAbort with "%#016llx:\n    %s\n    ...in %s at line %d"
    void FatalReport() const override;

    // Message string and source location fields
};

} // namespace NRadEngine

#endif // PNSRAD_CORE_CBASEERR_H
