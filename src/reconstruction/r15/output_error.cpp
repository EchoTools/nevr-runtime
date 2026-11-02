// NRadEngine::OutputError
// Quest libr15.so VA 0x12d1b38; ReVault binary 576.
// ReVault raw parent 100219890, reconstruction node 100600599; grade M: all 317 instructions checked to the ret at 0x12d2028; 1 placeholder stays (DAT_03861210, VA 0x3861210); the record table and the text serializer tail use raw offsets.
// @0x12d1b38 — NRadEngine::OutputError
/* @confidence: M */

#include <cstddef>
#include <cstdint>
#include <cstring>

// Placeholder: the owner object the TLS handle is looked up in (VA 0x3861210), read as the CEnvironment this-pointer. The tree records it as "name not recovered".
namespace NRadEngine { class CEnvironment; }
extern NRadEngine::CEnvironment* DAT_03861210;
// g_vptr_CTextStreamSerializerWrite (merged convention g_vptr_<Class>): vptr value of NRadEngine::CTextStreamSerializerWrite, the vtable address point, vtable_va 0x372eba8 (decompiler label PTR__CTextStreamSerializerWrite_0372eba8).
extern const std::byte g_vptr_CTextStreamSerializerWrite[];

namespace NRadEngine {

// NRadEngine::CSymbol64: one 8-byte word here (PushContext and TypeName pass it as a long).
using CSymbol64 = std::uint64_t;

struct CAllocator;
struct CStream;  // base of CConsoleStream at +0 (ReVault class hierarchy); only its address is passed

struct CMemoryContext {
    // NRadEngine::CMemoryContext::context: its first word is the default allocator.
    static void* context;
    static CAllocator* CurrentPtr();                // 0x12cae90 (thunk 0xffbc50)
    static void SetCurrent(CAllocator* allocator);  // 0x12cafac (thunk 0xfd3590)
};

namespace CMemory {
void Fill(void* destination, unsigned char value, std::uint64_t count);  // 0x12cafcc (thunk 0x1047300)
}  // namespace CMemory

class CSysThread {
public:
    static void* GetTLS(std::int32_t key);             // 0x108a22c (thunk 0x104b720)
    static void SetTLS(std::int32_t slot, void* value);  // 0x108a228 (thunk 0x101c580)
};

class CEnvironment {
public:
    // this is the owner (x0); NRadEngine::CEnvironment::GetGlobal @0x1092f2c (thunk 0xff58f0)
    void* GetGlobal(std::uint64_t key, std::uint64_t arg2);
    // this is the owner (x0); NRadEngine::CEnvironment::GetTlsHandle @0x1092de8 (thunk 0xff6260)
    std::int32_t GetTlsHandle(std::uint64_t key, std::uint64_t arg2);
};

class CErrorDb;
template <typename Owner, std::uint64_t Index>
class CEnvData;
template <>
class CEnvData<CErrorDb, 0> {
public:
    // NRadEngine::CEnvData<NRadEngine::CErrorDb,0ull>::tls: the TLS handle, -1 until a block is bound.
    static std::int32_t tls;
};

template <std::size_t N>
struct CFixedStringT {
    char text[N];
};
static_assert(sizeof(CFixedStringT<2048>) == 0x800);

class CSysOS {
public:
    // NRadEngine::CSysOS::CurrentCallstack, 0x10806d8 (thunk 0xfd9fe0).
    static void CurrentCallstack(CFixedStringT<2048>& out, std::uint64_t skip, std::uint64_t max_frames,
                                 const char* separator, std::uint32_t flags);
};

// NRadEngine::ProxyHandleError(int const&), 0x12cd820 (thunk 0x1051790).
void ProxyHandleError(const std::int32_t& status);

namespace NWriteLog {
enum ELogLevel : std::int32_t {};
// NRadEngine::NWriteLog::WriteLog, 0x1088c20 (thunk 0x104b960). Variadic.
void WriteLog(ELogLevel level, unsigned long long channel, const char* format, ...);
}  // namespace NWriteLog

using BreakpointHandler = void (*)();
// The trap helper the tree uses for brk #1 (cmemory_context.hpp).
BreakpointHandler SoftwareBreakpoint(int kind, std::uintptr_t address);

// NRadEngine::CBaseErr: the record object stored at env + 0x40008 (vtable 0x372e418).
class CBaseErr {
public:
    virtual ~CBaseErr();
    // vtable slot 2 (0x10): Serialize, 0x12ce89c. The serializer is passed by address.
    virtual std::int32_t Serialize(void* serializer);
    // vtable slot 3 (0x18): TypeName() const. Pure in CBaseErr; CBasicErr::TypeName is at 0x12d35fc.
    virtual CSymbol64 TypeName() const = 0;
};

// NRadEngine::CConsoleStream: 0x50 bytes in this frame (sp+0x160). Ctor 0x101ee70, dtor 0x104ddc0. CStream is at +0.
class CConsoleStream {
public:
    CConsoleStream(NWriteLog::ELogLevel level, CAllocator* allocator);
    ~CConsoleStream();
    CStream* AsStream() { return reinterpret_cast<CStream*>(this); }
    alignas(8) std::byte storage[0x50];
};
static_assert(sizeof(CConsoleStream) == 0x50);

// The serializers are built in place at sp+0x8, 0x158 bytes before CConsoleStream. Their layout is not recovered.
// The code touches +0x0 (vtable word) and, on the text path, the CMemBlock at +0x18 with its flags at +0x34.
constexpr std::size_t kSerializerBytes = 0x158;

class CJsonStreamSerializerWrite {
public:
    explicit CJsonStreamSerializerWrite(CStream* stream);  // ctor 0xff9aa0 (vtable 0x372e9b0)
    ~CJsonStreamSerializerWrite();                          // 0x103a240 (vtable slot 0 at 0x12edf1c)
    alignas(8) std::byte storage[kSerializerBytes];
};

class CTextStreamSerializerWrite {
public:
    CTextStreamSerializerWrite(CStream* stream, std::int32_t mode);  // ctor 0xfda210 (vtable 0x372eba8); mode is the literal 1
    alignas(8) std::byte storage[kSerializerBytes];
};

// NRadEngine::CMemBlock; its flags byte is at +0x1c. Detach 0x12dc2f8 (thunk 0xfe2470); dtor 0x12dbd40 (thunk 0x100e200).
class CMemBlock {
public:
    void Detach();
    ~CMemBlock();
};

namespace {

// Symbol key read from rodata 0x2c2f758 (bytes 64 80 ed 26 05 86 e8 35).
constexpr std::uint64_t kCErrorDbKey = 0x35e8860526ed8064ULL;
// Record table at env + 0x40008: 0x18-byte records { CBaseErr* at +0x0, int at +0x8, int at +0xc }.
constexpr std::uint64_t kRecordStride = 0x18;
// CMemBlock offset inside CTextStreamSerializerWrite.
constexpr std::uint64_t kTextBlockOffset = 0x18;
// Vtable byte offsets: 0x98 is PushContext (slot 19), 0xa8 is PopContext (slot 21), same in both serializers.
constexpr std::uint64_t kPushContextSlot = 0x98;
constexpr std::uint64_t kPopContextSlot = 0xa8;

template <typename T>
T LoadAt(const std::byte* base, std::uint64_t offset) {
    T value{};
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

// Calls the virtual at byte offset slot_offset in the vtable of the object at `object`.
template <typename Result, typename... Args>
Result CallVirtual(void* object, std::uint64_t slot_offset, Args... args) {
    const std::byte* const vtable = LoadAt<const std::byte*>(static_cast<const std::byte*>(object), 0);
    void* const slot = LoadAt<void*>(vtable, slot_offset);
    return reinterpret_cast<Result (*)(void*, Args...)>(slot)(object, args...);
}

// The TLS binding block the binary repeats (0x12d1b6c-0x12d1c04, 0x12d1d1c, 0x12d1d70, 0x12d1e40, 0x12d1f48).
std::byte* RefreshTlsBinding() {
    std::int32_t handle = CEnvData<CErrorDb, 0>::tls;
    std::byte* value = nullptr;
    if (handle == -1) {
        if (DAT_03861210 != nullptr) {
            handle = DAT_03861210->GetTlsHandle(kCErrorDbKey, 0);
            CEnvData<CErrorDb, 0>::tls = handle;
            value = static_cast<std::byte*>(CSysThread::GetTLS(handle));
        }
    } else {
        value = static_cast<std::byte*>(CSysThread::GetTLS(handle));
    }
    if (value == nullptr && DAT_03861210 != nullptr) {
        value = static_cast<std::byte*>(DAT_03861210->GetGlobal(kCErrorDbKey, 0));
        CSysThread::SetTLS(handle, value);
    }
    return value;
}

// The record write both serializer paths share: TypeName, PushContext, Serialize, then PopContext when pushed.
// Binary: 0x12d1ddc-0x12d1e2c (JSON path) and 0x12d1ee4-0x12d1f2c (text path).
template <typename Writer>
std::int32_t WriteRecord(CBaseErr* sink, Writer& writer) {
    RefreshTlsBinding();
    const CSymbol64 type_name = sink->TypeName();
    const std::int32_t pushed = CallVirtual<std::int32_t>(&writer, kPushContextSlot, type_name);
    const std::int32_t record_status = sink->Serialize(&writer);
    if (pushed != 0) {
        CallVirtual<void>(&writer, kPopContextSlot);
    }
    return record_status;
}

}  // namespace

std::int32_t OutputError(std::int32_t status, NWriteLog::ELogLevel level) {
    // 0x12d1b54-0x12d1b68: status + 1 (unsigned) below 2, i.e. status is 0 or -1: return 0, nothing written.
    if (static_cast<std::uint32_t>(status) + 1U < 2U) {
        return 0;
    }
    // 0x12d1b5c: the status word at sp+0x9bc (local_54), which ProxyHandleError takes by reference below.
    std::int32_t status_slot = status;

    // 0x12d1b6c-0x12d1c04: bind the per-thread record block (pvVar3).
    std::byte* const env = RefreshTlsBinding();
    // 0x12d1c10: the JSON flag *(int*)(env + 0x4030c), read before the callstack is taken.
    const std::int32_t json = LoadAt<std::int32_t>(env, 0x4030c);

    // 0x12d1c18-0x12d1c54: callstack into the 2048-byte buffer at sp+0x1b0, logged at level 8.
    CFixedStringT<2048> callstack;
    CMemory::Fill(callstack.text, 0, 0x800);
    CSysOS::CurrentCallstack(callstack, 2, 0x10, "\n", 1);
    NWriteLog::WriteLog(static_cast<NWriteLog::ELogLevel>(8), 0ULL, "Callstack:\n%s", callstack.text);

    // 0x12d1c58-0x12d1c7c: no memory context means a level-8 log line and the brk #1 trap (SoftwareBreakpoint form).
    if (CMemoryContext::context == nullptr) {
        NWriteLog::WriteLog(static_cast<NWriteLog::ELogLevel>(8), 0ULL,
                            "Trying to use memory context before it's initialized!");
        SoftwareBreakpoint(1, 0x12d1c80)();
    }
    // 0x12d1c84-0x12d1ca8: the default allocator is the context's first word; the current one is saved and replaced.
    CAllocator* const default_allocator = *static_cast<CAllocator**>(CMemoryContext::context);
    CAllocator* const previous_allocator = CMemoryContext::CurrentPtr();
    CMemoryContext::SetCurrent(default_allocator);
    {
        // 0x12d1c98-0x12d1ca8: CConsoleStream(sp+0x160, level, CurrentPtr()).
        CConsoleStream stream(level, CMemoryContext::CurrentPtr());

        // 0x12d1cac-0x12d1d04: record index status - 1 at env + 0x40008. A record is null unless its +0x8 int,
        // +0x0 pointer and +0xc int are all set. The three tests use the 32-bit unsigned index, the load a signed one.
        const std::uint32_t index = static_cast<std::uint32_t>(status) - 1U;
        const std::uint64_t record_u = static_cast<std::uint64_t>(index) * kRecordStride;
        const auto signed_index = static_cast<std::int32_t>(index);
        const std::uint64_t record_s =
            static_cast<std::uint64_t>(static_cast<std::int64_t>(signed_index) * static_cast<std::int64_t>(kRecordStride));
        CBaseErr* sink = nullptr;
        if (LoadAt<std::int32_t>(env, record_u + 0x40010) != 0 &&
            LoadAt<std::byte*>(env, record_u + 0x40008) != nullptr &&
            LoadAt<std::int32_t>(env, record_u + 0x40014) != 0) {
            sink = LoadAt<CBaseErr*>(env, record_s + 0x40008);
        }

        if (json != 0) {
            // 0x12d1d6c: CJsonStreamSerializerWrite(sp+0x8, &stream). Its destructor runs at the end of this block (0x12d1eac).
            CJsonStreamSerializerWrite writer(stream.AsStream());
            const std::int32_t record_status = WriteRecord(sink, writer);  // local_58 (sp+0x9b8)
            if (record_status != 0) {
                ProxyHandleError(record_status);  // 0x12d1e30
            }
            RefreshTlsBinding();  // 0x12d1e40-0x12d1eac
        } else {
            // 0x12d1d14: CTextStreamSerializerWrite(sp+0x8, &stream, 1).
            CTextStreamSerializerWrite writer(stream.AsStream(), 1);
            const std::int32_t record_status = WriteRecord(sink, writer);
            if (record_status != 0) {
                ProxyHandleError(record_status);  // 0x12d1f38-0x12d1f44
            }
            RefreshTlsBinding();  // 0x12d1f48-0x12d1fb0
            // 0x12d1fb4-0x12d1fe0: the inlined tail of ~CTextStreamSerializerWrite. The flags are read first, then the
            // vtable word is reset, then the CMemBlock at +0x18 is detached when flags bits 1-2 are set, and destroyed.
            const std::byte flags = writer.storage[kTextBlockOffset + 0x1c];
            *reinterpret_cast<const std::byte**>(writer.storage) = g_vptr_CTextStreamSerializerWrite;
            CMemBlock* const block = reinterpret_cast<CMemBlock*>(writer.storage + kTextBlockOffset);
            if ((std::to_integer<std::uint8_t>(flags) & 6U) != 0) {
                block->Detach();
            }
            block->~CMemBlock();
        }
    }  // ~CConsoleStream, 0x104ddc0 (0x12d1fe8)

    // 0x12d1ff0-0x12d1ff8: restore the previous allocator, then ProxyHandleError(&status).
    CMemoryContext::SetCurrent(previous_allocator);
    ProxyHandleError(status_slot);
    // 0x12d2000: the status word is set to -1 before the return of 1. Nothing reads it afterwards.
    status_slot = -1;
    return 1;
}

}  // namespace NRadEngine
