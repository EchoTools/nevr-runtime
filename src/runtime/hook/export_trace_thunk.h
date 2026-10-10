#pragma once
// The export tracer's thunk layer (#20): a generated stub per traced export that forwards to the real
// function and records one call into a lock-free ring.
//
// What a traced call costs and does, so it can be left out of any timing argument:
//   - the stub is `mov r11, <record>; jmp NevrTraceEntry`; NevrTraceEntry (the assembly in the .cpp) saves the
//     four argument registers, copies the caller's first 12 stack arguments, reads the time stamp counter,
//     calls the original with the registers restored, reads the counter again, and pushes one fixed-size
//     CallRecord (core/call_ring.h). No log call, no allocation, no lock, no system call on the game's thread.
//   - arguments in rcx, rdx, r8, r9 and the first 12 stack slots reach the original unchanged; the return value
//     is rax or xmm0, both handed back untouched; the nonvolatile registers are preserved; the frame has
//     Windows x64 unwind data, so an exception that unwinds through a traced call still unwinds.
//   - the tracer records four integer arguments and rax/xmm0 raw: it does not know the signatures, so it never
//     dereferences an argument. An export taking more than 12 stack arguments would read garbage past the copy
//     (none of pnsrad's or pnsovr's does: the widest takes four register arguments).
//
// A thunk is made per (module, name) the game resolves, so exports that identical-code folding put at one
// address (pnsrad's four Mic* queries) stay distinct: each name has its own record and id.

#include <cstdint>

#include "core/call_ring.h"

namespace ExportTrace {

/// Most thunks one process can hold. Pnsrad exports 41 names and pnsovr about 52; the game resolves each once.
constexpr std::uint32_t kMaxThunks = 256;

/// The ring's capacity: records between two drains. At 250 ms drains this is 16k calls per second per
/// process before a record is dropped (and counted).
constexpr std::size_t kRingRecords = 4096;

/// Prepares the ring (once, before the first MakeThunk). Safe to call again; it resets the ring and the
/// thunk table for a test, and must not run while a traced call is in flight.
void Reset();

/// A callable that forwards to `original` and records the call under `id`. Null when the table or the stub
/// memory is exhausted; the caller then uses `original` (an untraced export is never a broken one).
void* MakeThunk(void* original, std::uint32_t id);

/// The single consumer. False when no record is ready.
bool Pop(nevr::CallRecord* out);

std::uint64_t Pushed();
std::uint64_t Dropped();

}  // namespace ExportTrace
