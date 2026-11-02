// NRadEngine::TranslateWwiseResult
// Quest libr15.so VA 0x26483dc; ReVault binary 576.
// ReVault raw parent 100319745, reconstruction node 100600785; grade M because of the one g_vptr_CErrMsg extern (GOT slot 0x382b7f8 plus 0x10 = vtable address point 0x372e3d8, slot zero on disk so not byte-checked); all 275 instructions traced to the ret at 0x2648824, the 62 case labels of the AKRESULT switch listed with their string addresses, every callee named, WriteError a virtual member at slot 5 of the CErrMsg vtable (slots 0 to 4 are placeholder virtuals named from the vtable listing), and the database record is raw byte offsets.
// @0x26483dc — NRadEngine::TranslateWwiseResult
/* @confidence: M */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

// g_vptr_CErrMsg (merged convention g_vptr_<Class>): the vptr value of NRadEngine::CErrMsg, the vtable address
// point, vtable_va 0x372e3d8 (ReVault class table; decompiler label PTR__CBaseErr_0372e3d8). The binary reads the
// loader-filled GOT slot at 0x382b7f8 and adds 0x10 (0x26487a8..0x26487c4); that slot is zero on disk.
extern const std::byte g_vptr_CErrMsg[];

namespace NRadEngine {

// NRadEngine::CSymbol64: one 8-byte word in these signatures.
using CSymbol64 = std::uint64_t;

namespace NWriteLog {
enum ELogLevel : std::int32_t {};
// NRadEngine::NWriteLog::WriteLog(ELogLevel, unsigned long long, char const*, ...) @0x1088c20 (thunk 0x104b960).
void WriteLog(ELogLevel level, unsigned long long channel, const char* format, ...);
}  // namespace NWriteLog

// NRadEngine::CBaseErr: root of the error classes (ReVault hierarchy CErrMsg -> CBasicErr -> CBaseErr). The objects
// are polymorphic in the binary; only the virtual slots of CErrMsg are declared, on CErrMsg.
class CBaseErr {
public:
    // NRadEngine::CBaseErr::ErrorCode() const @0x12ce4d4 (thunk 0xfea5b0): returns the 8-byte symbol at +0x10.
    CSymbol64 ErrorCode() const;
};

class CBasicErr : public CBaseErr {
public:
    // NRadEngine::CBasicErr::CBasicErr() @0x12cef08 (thunk 0xfda680): sets +0x10 to -1, the int at +0x8 to -1, the
    // CBasicErr vptr, and zero-fills +0x18 (0x400 bytes), +0x418 (0x200 bytes) and the word at +0x618.
    CBasicErr();
};

class CErrMsg : public CBasicErr {
public:
    // NRadEngine::CErrMsg::CErrMsg(char const*, unsigned int, NRadEngine::CSymbol64, char const*, ...) @0x12cdb78
    // (thunk 0x1030780): builds the error, formatting the message text into the 0x800 bytes at +0x620.
    CErrMsg(const char* file, unsigned int line, CSymbol64 symbol, const char* format, ...);

    // Vtable of CErrMsg (ReVault vtable_va 0x372e3d8), slots in order. Slots 0 to 4 are placeholders that keep
    // WriteError at slot 5 (offset 0x28); the binary's names are in each comment and none of them is called here.
    virtual void Slot0();  // ~CBaseErr @ 0x12ce69c (offset 0x0)
    virtual void Slot1();  // ~CErrMsg @ 0x12d3610 (offset 0x8)
    virtual void Slot2();  // Serialize @ 0x12ce0f4 (offset 0x10)
    virtual void Slot3();  // TypeName @ 0x12d3614 (offset 0x18)
    virtual void Slot4();  // Msg @ 0x12cd5c4 (offset 0x20)
    // NRadEngine::CErrMsg::WriteError @ 0x12ce4b0, slot 5 (offset 0x28): the error object writes itself out.
    virtual void WriteError();
};

class CErrorDb {
public:
    // NRadEngine::CErrorDb::AddRecordToTop(NRadEngine::CSymbol64, unsigned int, unsigned int*) @0x12cf074
    // (thunk 0x10353b0): reserves a record of `size` bytes, stores its index through `index`, returns 0 on success.
    std::int32_t AddRecordToTop(CSymbol64 symbol, unsigned int size, unsigned int* index);
};

class CMemory {
public:
    // NRadEngine::CMemory::Fill(void*, unsigned char, unsigned long long) @0x12cafcc (thunk 0x1047300).
    static void Fill(void* destination, unsigned char value, unsigned long long count);
};

// NRadEngine::GetErrorDb() @0x12d33f0 (thunk 0xfd4240): the per-thread error database, or null.
void* GetErrorDb();

// NRadEngine::ProxyHandleError(int const&) @0x12cd820 (thunk 0x1051790).
void ProxyHandleError(const std::int32_t& status);

namespace {

// The CErrMsg object is 0xe20 bytes: the frame of the original is sub sp, 0xe20 and the same size is requested
// from AddRecordToTop (0x264876c).
constexpr std::size_t kErrMsgSize = 0xe20;
// +0x08: int (-1) and +0x10: the CSymbol64 error id, copied as one 16-byte block (0x26487cc, 0x26487dc).
constexpr std::size_t kHeaderOffset = 0x08;
constexpr std::size_t kHeaderSize = 0x10;
// +0x18: the 0x608 bytes the CBasicErr constructor initialises (0x400 + 0x200 + 8), copied with memcpy (0x26487e0).
constexpr std::size_t kBodyOffset = 0x18;
constexpr std::size_t kBodySize = 0x608;
// +0x620: the 0x800-byte message text (0x26487b4, 0x26487e8).
constexpr std::size_t kTextOffset = 0x620;
constexpr std::size_t kTextSize = 0x800;
static_assert(kHeaderOffset + kHeaderSize == kBodyOffset);
static_assert(kBodyOffset + kBodySize == kTextOffset);
static_assert(kTextOffset + kTextSize == kErrMsgSize);

// The record table of the error database: 0x18-byte entries from +0x40008, the first word of an entry is the
// record pointer (0x264878c..0x264879c).
constexpr std::uint64_t kRecordStride = 0x18;
constexpr std::size_t kRecordPointerOffset = 0x40008;

// The CSymbol64 error id passed to CErrMsg: the 8 bytes at VA 0x32ed0e8 (43 88 e0 f5 14 e8 e0 ba), loaded at
// 0x2648718..0x2648724.
constexpr CSymbol64 kErrorSymbol = 0xbae0e814f5e08843ULL;

// Raw storage for the CErrMsg built on the stack (the original's sp..sp+0xe20).
struct ErrMsgStorage {
    alignas(8) std::byte bytes[kErrMsgSize];
};
static_assert(sizeof(ErrMsgStorage) == 0xe20);

}  // namespace

// @0x26483dc — NRadEngine::TranslateWwiseResult
/* @confidence: M */
std::uint64_t TranslateWwiseResult(const char* label, std::uint32_t result, const char* file, std::uint32_t line) {
    // 0x26483ec, 0x2648400: the default message (VA 0x32ec63c) is in force for result > 0x5d and for every value the
    // jump table at 0x32ec1e0 sends to 0x2648700 (6, 10..13, 18, 19, 24..30, 32..34, 42, 47..51, 55, 58..62, 65, 68, 70).
    const char* message = "Unknown Wwise error.";
    switch (result) {
    case 0x00: message = "This feature is not implemented."; break;  // 0x32ec651
    case 0x01: return 0;  // table entry 0xfb: 0x2648814 with w0 = wzr, nothing logged
    case 0x02: message = "The operation failed."; break;  // 0x32ec672
    case 0x03: message = "The operation succeeded partially."; break;  // 0x32ec688
    case 0x04: message = "Incompatible formats."; break;  // 0x32ec6ab
    case 0x05: message = "The stream is already connected to another node."; break;  // 0x32ec6c1
    case 0x07: message = "An unexpected value causes the file to be invalid."; break;  // 0x32ec6f2
    case 0x08: message = "The file header is too large."; break;  // 0x32ec725
    case 0x09: message = "The maximum was reached."; break;  // 0x32ec743
    case 0x0e: message = "The ID is invalid."; break;  // 0x32ec75c
    case 0x0f: message = "The ID was not found."; break;  // 0x32ec76f
    case 0x10: message = "The InstanceID is invalid."; break;  // 0x32ec785
    case 0x11: message = "No more data is available from the source."; break;  // 0x32ec7a0
    case 0x14: message = "The StateGroup is not a valid channel."; break;  // 0x32ec7cb
    case 0x15: message = "The child already has a parent."; break;  // 0x32ec7f2
    case 0x16: message = "The language is invalid (applies to the Low-Level I/O)."; break;  // 0x32ec812
    case 0x17: message = "It is not possible to add itself as its own child."; break;  // 0x32ec84a
    case 0x1f: message = "Something is not within bounds."; break;  // 0x32ec87d
    case 0x23: message = "The item could not be added because it was already in the list."; break;  // 0x32ec89d
    case 0x24: message = "This path is not known."; break;  // 0x32ec8dd
    case 0x25: message = "Stuff in vertices before trying to start it."; break;  // 0x32ec8f5
    case 0x26: message = "Only a running path can be paused."; break;  // 0x32ec922
    case 0x27: message = "Only a paused path can be resumed."; break;  // 0x32ec945
    case 0x28: message = "This path is already there."; break;  // 0x32ec968
    case 0x29: message = "This path is not there."; break;  // 0x32ec984
    case 0x2b: message = "The consumer needs more."; break;  // 0x32ec99c
    case 0x2c: message = "The consumer does not need more."; break;  // 0x32ec9b5
    case 0x2d: message = "The provider has available data."; break;  // 0x32ec9d6
    case 0x2e: message = "The provider does not have available data."; break;  // 0x32ec9f7
    case 0x34: message = "Memory error."; break;  // 0x32eca22
    case 0x35: message = "The requested action was cancelled (not an error)."; break;  // 0x32eca30
    case 0x36: message = "Trying to load a bank using an ID which is not defined."; break;  // 0x32eca63
    case 0x38: message = "Error while reading a bank."; break;  // 0x32eca9b
    case 0x39: message = "Invalid switch type (used with the switch container)."; break;  // 0x32ecab7
    case 0x3f: message = "Source format not known yet."; break;  // 0x32ecaed
    case 0x40: message = "The bank version is not compatible with the current bank reader."; break;  // 0x32ecb0a
    case 0x42: message = "File not found."; break;  // 0x32ecb4b
    case 0x43: message = "Either the device doesn't exist or is disabled."; break;  // 0x32ecb5b
    case 0x45: message = "The bank load failed because the bank is already loaded."; break;  // 0x32ecb8b
    case 0x47: message = "The effect on the node is rendered."; break;  // 0x32ecbc4
    case 0x48: message = "A routine needs to be executed on some CPU."; break;  // 0x32ecbe8
    case 0x49: message = "The executed routine has finished its execution."; break;  // 0x32ecc14
    case 0x4a: message = "The memory manager should have been initialized at this point."; break;  // 0x32ecc45
    case 0x4b: message = "The stream manager should have been initialized at this point."; break;  // 0x32ecc84
    case 0x4c: message = "The machine does not support SSE instructions (required on PC)."; break;  // 0x32eccc3
    case 0x4d: message = "The system is busy and could not process the request."; break;  // 0x32ecd03
    case 0x4e: message = "Channel configuration is not supported in the current execution context."; break;  // 0x32ecd39
    case 0x4f: message = "Plugin media is not available for effect."; break;  // 0x32ecd82
    case 0x50: message = "Sound was Not Allowed to play."; break;  // 0x32ecdac
    case 0x51: message = "SDK command is too large to fit in the command queue."; break;  // 0x32ecdcb
    case 0x52: message = "A play request was rejected due to the MIDI filter parameters."; break;  // 0x32ece01
    case 0x53: message = "Detected incompatibility between Custom platform of banks and of connected app."; break;  // 0x32ece40
    case 0x54: message = "DLL could not be loaded, either because it is not found or one dependency is missing."; break;  // 0x32ece90
    case 0x55: message = "Plugin DLL search path could not be found."; break;  // 0x32ecee6
    case 0x56: message = "No Java VM provided in AkInitSettings."; break;  // 0x32ecf11
    case 0x57: message = "OpenSL returned an error.  Check error log for more details."; break;  // 0x32ecf38
    case 0x58: message = "Plugin not registered, implement AK::PluginRegistration and use AK_STATIC_LINK_PLUGIN"; break;  // 0x32ecf75
    case 0x59: message = "A pointer to audio data was not aligned (AkTypes.h in the platform-specific folder)."; break;  // 0x32ecfcb
    case 0x5a: message = "Incompatible Audio device."; break;  // 0x32ed020
    case 0x5b: message = "Two Wwise objects share the same ID."; break;  // 0x32ed03b
    case 0x5c: message = "The Init bank was not loaded yet, the sound engine isn't ready yet."; break;  // 0x32ed060
    case 0x5d: message = "The specified device ID does not match any of the output devices "; break;  // 0x32ed0a4
    default: break;
    }

    // 0x2648700..0x2648714: WriteLog(8, 0, "%s: %s" (0x2d4780e), label, message).
    NWriteLog::WriteLog(static_cast<NWriteLog::ELogLevel>(8), 0, "%s: %s", label, message);

    // 0x2648718..0x2648740: CErrMsg(sp, file, line, kErrorSymbol, "%s" (0x2d53c79), message).
    ErrMsgStorage storage;
    CErrMsg* const error = new (storage.bytes) CErrMsg(file, line, kErrorSymbol, "%s", message);

    // 0x2648744..0x2648748: no error database means the error object writes itself out.
    void* const raw_db = GetErrorDb();
    if (raw_db == nullptr) {
        // 0x2648800..0x2648810: slot 5 (WriteError) of the object's own vtable, called with the object as this,
        // then w0 = -1.
        error->WriteError();
        return 0xffffffffULL;
    }

    // 0x264874c..0x2648774: the record index starts at -1 and AddRecordToTop(db, error id, 0xe20, &index) fills it.
    auto* const db = static_cast<CErrorDb*>(raw_db);
    unsigned int index = 0xffffffffU;
    const std::int32_t status = db->AddRecordToTop(error->ErrorCode(), static_cast<unsigned int>(kErrMsgSize), &index);

    // 0x2648778..0x264877c: a nonzero status is the return value (w0, zero-extended).
    if (status != 0) {
        return static_cast<std::uint32_t>(status);
    }

    // 0x2648780..0x2648784: ProxyHandleError(&status).
    ProxyHandleError(status);

    // 0x2648788..0x264879c: the record pointer is the first word of entry `index` in the table at db + 0x40008.
    std::byte* record = nullptr;
    std::memcpy(&record, static_cast<const std::byte*>(raw_db) + static_cast<std::uint64_t>(index) * kRecordStride + kRecordPointerOffset,
                sizeof record);

    // 0x26487a0..0x26487a4: CBasicErr() on the record.
    new (record) CBasicErr();

    // 0x26487a8..0x26487c4: the record's vptr is g_vptr_CErrMsg.
    const std::byte* const vptr = g_vptr_CErrMsg;
    std::memcpy(record, &vptr, sizeof vptr);

    // 0x26487b0..0x26487c8: the 0x800 message bytes at +0x620 are cleared.
    CMemory::Fill(record + kTextOffset, 0, kTextSize);

    // 0x26487cc, 0x26487dc: the 16 bytes at +0x08 (the int and the error id).
    std::memcpy(record + kHeaderOffset, storage.bytes + kHeaderOffset, kHeaderSize);

    // 0x26487d0..0x26487e0: memcpy(record + 0x18, local + 0x18, 0x608).
    std::memcpy(record + kBodyOffset, storage.bytes + kBodyOffset, kBodySize);

    // 0x26487e4..0x26487f0: memcpy(record + 0x620, local + 0x620, 0x800).
    std::memcpy(record + kTextOffset, storage.bytes + kTextOffset, kTextSize);

    // 0x26487f4..0x26487fc: return index + 1, read back from the stack slot (zero-extended).
    return static_cast<std::uint64_t>(index + 1U);
}

}  // namespace NRadEngine
