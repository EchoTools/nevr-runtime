#include "runtime/hook/export_trace_thunk.h"

#include <windows.h>

#include <cstddef>
#include <cstring>
#include <mutex>

namespace {

// What NevrTraceEntry fills for NevrTraceCommit. The assembly addresses these fields by offset
// (rec 0, args 8, ret 40, t0 48, t1 56, xmm0 64): keep the two in step.
struct TraceFrame {
  const void* record;
  std::uint64_t args[4];
  std::uint64_t ret;
  std::uint64_t t0;
  std::uint64_t t1;
  std::uint64_t xmm0[2];  // all 128 bits: a __m128 result is returned whole
};
static_assert(offsetof(TraceFrame, args) == 8 && offsetof(TraceFrame, ret) == 40 &&
                  offsetof(TraceFrame, t0) == 48 && offsetof(TraceFrame, t1) == 56 &&
                  offsetof(TraceFrame, xmm0) == 64 && sizeof(TraceFrame) == 80,
              "NevrTraceEntry's frame offsets");

// One per thunk, addressed by the stub through r11. `original` is first: the assembly calls [r11].
struct ThunkRecord {
  void* original;
  std::uint32_t id;
  std::uint32_t pad;
};
static_assert(offsetof(ThunkRecord, original) == 0, "NevrTraceEntry calls [record]");

// Static storage, zero-initialized, no dynamic initializer (a tracer that is off costs nothing).
ThunkRecord g_records[nevr_export_trace::kMaxThunks];
nevr::CallRing<nevr_export_trace::kRingRecords> g_ring;
std::uint32_t g_thunkCount = 0;
unsigned char* g_stubPool = nullptr;

constexpr std::size_t kStubBytes = 32;  // 10 (mov r11, imm64) + 6 (jmp [rip+0]) + 8 (the target), padded
std::mutex& MakeMutex() {
  static std::mutex* const m = new std::mutex();
  return *m;
}

}  // namespace

extern "C" {

void NevrTraceEntry();

// Called by NevrTraceEntry after the original returned, on the game's thread. Pushes and returns: nothing
// here may block, log, allocate or throw.
void NevrTraceCommit(const void* raw) {
  const TraceFrame* const frame = static_cast<const TraceFrame*>(raw);
  const ThunkRecord* const record = static_cast<const ThunkRecord*>(frame->record);
  nevr::CallRecord out;
  out.enterTicks = frame->t0;
  out.exitTicks = frame->t1;
  for (int i = 0; i < 4; ++i) out.args[i] = frame->args[i];
  out.ret = frame->ret;
  out.retXmm0 = frame->xmm0[0];
  out.exportId = record->id;
  out.threadId = GetCurrentThreadId();
  g_ring.Push(out);
}

}  // extern "C"

// The trampoline. On entry r11 is the ThunkRecord, and the registers and stack are exactly what the game
// left for the export: [rsp] return address, [rsp+8..0x28) shadow space, [rsp+0x28...) stack arguments.
//
// Frame (rsp after the prologue): 0..0x20 shadow space for the callee, 0x20..0x80 a copy of the caller's
// first 12 stack arguments, 0x80..0xD0 the TraceFrame (xmm0 whole, at 0xC0). rbx is saved and holds the
// record across the call. Before the original is called nothing is called and no xmm register is touched, so
// the xmm arguments reach it as they were.
__asm__(R"(
    .text
    .p2align 4
    .globl NevrTraceEntry
    .def NevrTraceEntry; .scl 2; .type 32; .endef
    .seh_proc NevrTraceEntry
NevrTraceEntry:
    .intel_syntax noprefix
    push rbx
    .seh_pushreg rbx
    sub rsp, 0xD0
    .seh_stackalloc 0xD0
    .seh_endprologue
    mov rbx, r11
    mov qword ptr [rsp+0x80], rbx
    mov qword ptr [rsp+0x88], rcx
    mov qword ptr [rsp+0x90], rdx
    mov qword ptr [rsp+0x98], r8
    mov qword ptr [rsp+0xA0], r9
    mov rax, qword ptr [rsp+0x100]
    mov qword ptr [rsp+0x20], rax
    mov rax, qword ptr [rsp+0x108]
    mov qword ptr [rsp+0x28], rax
    mov rax, qword ptr [rsp+0x110]
    mov qword ptr [rsp+0x30], rax
    mov rax, qword ptr [rsp+0x118]
    mov qword ptr [rsp+0x38], rax
    mov rax, qword ptr [rsp+0x120]
    mov qword ptr [rsp+0x40], rax
    mov rax, qword ptr [rsp+0x128]
    mov qword ptr [rsp+0x48], rax
    mov rax, qword ptr [rsp+0x130]
    mov qword ptr [rsp+0x50], rax
    mov rax, qword ptr [rsp+0x138]
    mov qword ptr [rsp+0x58], rax
    mov rax, qword ptr [rsp+0x140]
    mov qword ptr [rsp+0x60], rax
    mov rax, qword ptr [rsp+0x148]
    mov qword ptr [rsp+0x68], rax
    mov rax, qword ptr [rsp+0x150]
    mov qword ptr [rsp+0x70], rax
    mov rax, qword ptr [rsp+0x158]
    mov qword ptr [rsp+0x78], rax
    rdtsc
    shl rdx, 32
    or rax, rdx
    mov qword ptr [rsp+0xB0], rax
    mov rcx, qword ptr [rsp+0x88]
    mov rdx, qword ptr [rsp+0x90]
    mov r8, qword ptr [rsp+0x98]
    mov r9, qword ptr [rsp+0xA0]
    call qword ptr [rbx]
    mov qword ptr [rsp+0xA8], rax
    movups xmmword ptr [rsp+0xC0], xmm0
    rdtsc
    shl rdx, 32
    or rax, rdx
    mov qword ptr [rsp+0xB8], rax
    lea rcx, [rsp+0x80]
    call NevrTraceCommit
    mov rax, qword ptr [rsp+0xA8]
    movups xmm0, xmmword ptr [rsp+0xC0]
    add rsp, 0xD0
    pop rbx
    ret
    .att_syntax prefix
    .seh_endproc
)");

namespace nevr_export_trace {

void Reset() {
  std::lock_guard<std::mutex> lock(MakeMutex());
  std::memset(g_records, 0, sizeof(g_records));
  g_thunkCount = 0;
  g_ring.Seed();
}

void* MakeThunk(void* original, std::uint32_t id) {
  if (original == nullptr) return nullptr;
  std::lock_guard<std::mutex> lock(MakeMutex());
  if (g_thunkCount >= kMaxThunks) return nullptr;
  if (g_stubPool == nullptr) {
    // Read-write-execute: other threads may be inside earlier stubs on this memory while a new one is
    // written, so the pages are never made non-executable between writes.
    void* const pool = VirtualAlloc(nullptr, kMaxThunks * kStubBytes, MEM_COMMIT | MEM_RESERVE,
                                    PAGE_EXECUTE_READWRITE);
    if (pool == nullptr) return nullptr;
    g_stubPool = static_cast<unsigned char*>(pool);
  }
  ThunkRecord& record = g_records[g_thunkCount];
  record.original = original;
  record.id = id;
  unsigned char* const stub = g_stubPool + static_cast<std::size_t>(g_thunkCount) * kStubBytes;
  const std::uint64_t recordAddress = reinterpret_cast<std::uint64_t>(&record);
  const std::uint64_t entryAddress = reinterpret_cast<std::uint64_t>(&NevrTraceEntry);
  std::size_t at = 0;
  stub[at++] = 0x49;  // mov r11, imm64
  stub[at++] = 0xBB;
  std::memcpy(stub + at, &recordAddress, 8);
  at += 8;
  stub[at++] = 0xFF;  // jmp qword ptr [rip+0]
  stub[at++] = 0x25;
  stub[at++] = 0x00;
  stub[at++] = 0x00;
  stub[at++] = 0x00;
  stub[at++] = 0x00;
  std::memcpy(stub + at, &entryAddress, 8);
  FlushInstructionCache(GetCurrentProcess(), stub, kStubBytes);
  ++g_thunkCount;
  return stub;
}

bool Pop(nevr::CallRecord* out) { return g_ring.Pop(out); }
std::uint64_t Pushed() { return g_ring.Pushed(); }
std::uint64_t Dropped() { return g_ring.Dropped(); }

}  // namespace nevr_export_trace
