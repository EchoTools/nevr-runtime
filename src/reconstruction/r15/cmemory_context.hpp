#pragma once

#include <cstdint>

namespace NRadEngine {

struct CAllocator {};

struct CMemoryContext {
    static void* context;
    static CAllocator* Current();
    static CAllocator* CurrentPtr();
    static void SetCurrent(CAllocator* allocator);
};

namespace NWriteLog {
void WriteLog(int severity, void* context, const char* message, ...);
}  // namespace NWriteLog

using BreakpointHandler = void (*)();
BreakpointHandler SoftwareBreakpoint(int kind, std::uintptr_t address);

}  // namespace NRadEngine
