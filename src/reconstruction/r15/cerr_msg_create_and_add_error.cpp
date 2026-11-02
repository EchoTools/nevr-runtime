// NRadEngine::CErrMsg::CreateAndAddError
// Quest libr15.so VA 0x12ce4dc; ReVault binary 576.
// ReVault raw parent 100219806, reconstruction node 100600596; grade M: all 112 instructions checked; 2 placeholders stay, DAT_03861210 (VA 0x3861210), read as the CEnvironment this-pointer, and DAT_0382b7f8 (VA 0x382b7f8), the loader-filled slot the vptr is read from.
// @0x12ce4dc — NRadEngine::CErrMsg::CreateAndAddError
/* @confidence: M */

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

namespace NRadEngine {

class CSysString {
public:
    // CSysString::VSNPrintF @0x108312c (thunk 0xfdd250). The va_list is passed by address (AAPCS64).
    static long VSNPrintF(char* buffer, unsigned long long size, const char* format, va_list args);
    // CSysString::Length @0x108188c (thunk 0xfd1370).
    static long Length(const char* buffer, unsigned long long size);
};

class CMemory {
public:
    // CMemory::Fill @0x12cafcc (thunk 0x1047300).
    static void Fill(void* destination, unsigned char value, std::uint64_t count);
};

class CSysThread {
public:
    // CSysThread::GetTLS @0x108a22c (thunk 0x104b720); CSysThread::SetTLS @0x108a228 (thunk 0x101c580).
    static void* GetTLS(std::int32_t slot);
    static void SetTLS(std::int32_t slot, void* value);
};

class CEnvironment {
public:
    // CEnvironment::GetTlsHandle @0x1092de8 (thunk 0xff6260); CEnvironment::GetGlobal @0x1092f2c (thunk 0xff58f0).
    std::int32_t GetTlsHandle(std::uint64_t symbol, std::uint64_t arg2);
    void* GetGlobal(std::uint64_t symbol, std::uint64_t arg2);
};

class CBasicErr {
public:
    // CBasicErr::CBasicErr @0x12cdc5c (thunk 0x1048eb0); `this` is the error object at offset 0.
    CBasicErr(const char* file, std::uint32_t line, std::uint64_t symbol);
};

class CErrMsg : public CBasicErr {
public:
    // The CErrMsg vtable has WriteError at slot 5, offset 0x28 (@0x12ce4b0).
    static int CreateAndAddError(const char* file, std::uint32_t line, std::uint64_t symbol,
                                 const char* format, ...);
};

class CErrorDb {
public:
    // CErrorDb::AddError<NRadEngine::CErrMsg> @0x12cd998 (thunk 0x10506e0); `this` comes from TLS.
    template <typename T>
    int AddError(T* error);
};

// CEnvData<NRadEngine::CErrorDb, 0ull>::tls: the TLS key slot (-1 until created), addressed through the GOT.
template <typename Owner, unsigned long long Index>
struct CEnvData {
    static std::int32_t tls;
};

}  // namespace NRadEngine

// Placeholder: DAT_03861210 (VA 0x3861210) holds the CEnvironment pointer; no name is recovered for it.
extern NRadEngine::CEnvironment* DAT_03861210;
// Placeholder: DAT_0382b7f8 (VA 0x382b7f8) is a loader-filled slot, zero on disk. CreateAndAddError loads its value and adds 0x10 to get the stored vptr.
// ReVault's vtable address for NRadEngine::CErrMsg is 0x372e3d8 (Ghidra label PTR__CBaseErr_0372e3d8). The +0x10 address point is expected to match it, but that binding is not byte-checked: the slot is zero on disk.
extern const std::byte* DAT_0382b7f8;

namespace {

// The CErrMsg object: CBasicErr at +0x00 (vptr at +0x00), the 0x800-byte message text at +0x620.
// ReVault records no class size; the original frame runs to +0xe20.
constexpr std::size_t kTextOffset = 0x620;
constexpr std::size_t kTextSize = 0x800;
constexpr std::size_t kObjectSize = kTextOffset + kTextSize;
// 8 bytes at VA 0x2c2f758: the key the environment is queried with.
constexpr std::uint64_t kTlsSymbol = 0x35e8860526ed8064ULL;

// Storage with no initializer: the binary does not zero the object. It Fills only the 0x800-byte text
// region, and the CBasicErr ctor @0x12cdc5c (called on the object first) is not reconstructed here.
struct ErrorObject {
    alignas(8) std::byte bytes[kObjectSize];
};
static_assert(sizeof(ErrorObject) == 0xe20);

// The TLS lookup at 0x12ce5d8..0x12ce6a0: the CErrorDb to add to, or nullptr for the WriteError path.
void* ResolveErrorDb() {
    std::int32_t& tls = NRadEngine::CEnvData<NRadEngine::CErrorDb, 0ULL>::tls;
    std::int32_t key = tls;
    if (key == -1) {
        if (DAT_03861210 == nullptr) {
            return nullptr;
        }
        key = DAT_03861210->GetTlsHandle(kTlsSymbol, 0);
        tls = key;
    }
    void* db = NRadEngine::CSysThread::GetTLS(key);
    if (db != nullptr) {
        return db;
    }
    if (DAT_03861210 == nullptr) {
        return nullptr;
    }
    db = DAT_03861210->GetGlobal(kTlsSymbol, 0);
    NRadEngine::CSysThread::SetTLS(key, db);
    return db;
}

}  // namespace

int NRadEngine::CErrMsg::CreateAndAddError(const char* file, std::uint32_t line,
                                           std::uint64_t symbol, const char* format, ...) {
    ErrorObject object;
    std::byte* const error = object.bytes;
    // The binary calls CBasicErr::CBasicErr @0x12cdc5c here. That ctor is not reconstructed in this file.
    new (error) NRadEngine::CBasicErr(file, line, symbol);
    // The binary loads the slot at 0x382b7f8, adds 0x10 and stores the result as the vptr (0x12ce55c..0x12ce574).
    const std::byte* const vptr = DAT_0382b7f8 + 0x10;
    std::memcpy(error, &vptr, sizeof vptr);

    std::byte* const text = error + kTextOffset;
    NRadEngine::CMemory::Fill(text, 0, kTextSize);
    if (format != nullptr) {
        va_list args;
        va_start(args, format);
        NRadEngine::CSysString::VSNPrintF(reinterpret_cast<char*>(text), kTextSize, format, args);
        va_end(args);
        const long length =
            NRadEngine::CSysString::Length(reinterpret_cast<const char*>(text), kTextSize);
        const auto used = static_cast<std::uint64_t>(length);
        if (used + 1 < kTextSize) {
            std::memset(text + used + 1, 0, kTextSize - 1 - used);
        }
    }

    if (void* const db = ResolveErrorDb(); db != nullptr) {
        return reinterpret_cast<NRadEngine::CErrorDb*>(db)->AddError<NRadEngine::CErrMsg>(
            reinterpret_cast<NRadEngine::CErrMsg*>(error));
    }

    // WriteError: slot 5 (offset 0x28) of the object's vtable, called with the object as `this`.
    const std::byte* stored_vptr = nullptr;
    std::memcpy(&stored_vptr, error, sizeof stored_vptr);
    void* write_error = nullptr;
    std::memcpy(&write_error, stored_vptr + 0x28, sizeof write_error);
    reinterpret_cast<void (*)(void*)>(write_error)(error);
    return -1;
}
