// CResourceManager::FillResourceFileRequest
// Quest libr15.so VA 0x26439f0; ReVault binary 576.
// ReVault raw parent 100319665, reconstruction node 100600473; grade M: every branch checked against the disassembly; GetTlsHandle and GetGlobal names taken from ReVault, semantics unverified; g_tls_owner unidentified.
// @0x26439f0 — CResourceManager::FillResourceFileRequest
/* @confidence: M */
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace q15t::r15::reconstruction {

// Callees by ReVault name, each with its VA. libr15 reaches most of them through
// adrp/ldr/br stubs; the stub address is given in parentheses where it differs.
void ClearFileRequestData(std::byte* request);                                                       // VA 0x1032c00
const char* String(const std::uint64_t* symbol);                                                     // VA 0x12ccb48 (stub 0x1038ec0)
std::int32_t GetTlsHandle(std::byte* owner, std::uint64_t symbol, std::uint64_t arg2);              // VA 0x1092de8 (stub 0xff6260)
void* GetGlobal(std::byte* owner, std::uint64_t symbol, std::uint64_t arg2);                        // VA 0x1092f2c (stub 0xff58f0)
void* GetTLS(std::int32_t key);                                                                      // VA 0x108a22c: b pthread_getspecific
void SetTLS(std::int32_t key, void* value);                                                          // VA 0x108a228: b pthread_setspecific
void SPrintF(char* buffer, const char* format, ...);                                                 // VA 0x108a460 (stub 0xfd8d70), 0x200-byte buffer
std::uint64_t CreateAndAddError(const char* file, std::int32_t line, std::uint64_t category,
                                const char* format, ...);                                            // VA 0x12ce4dc (stub 0xfd4870)

// Module data read by the body.
extern std::byte* g_tls_owner;  // VA 0x3861210: owner object for the TLS slot; name not recovered
extern std::int32_t g_tls_key;  // pthread key cache, addressed through GOT slot 0x3807390; -1 until created

namespace {

// 8-byte values and strings the body loads from fixed image addresses.
constexpr std::uint64_t kErrorCategory = 0xe8cc523d0fc9e5feULL;  // 8 bytes at VA 0x2c2f1f0
constexpr std::uint64_t kTlsSymbol = 0x35e8860526ed8064ULL;      // 8 bytes at VA 0x2c2f758
constexpr char kSourceFile[] =
    "d:\\projects\\rad\\dev\\src\\engine\\libs\\resource\\cresourcemanager.cpp";  // VA 0x32ebe0c
constexpr char kFormatKey[] = "0x%016llx.0x%016llx";                              // VA 0x32ebffe
constexpr char kMissingFormat[] =
    "Cannot find resource %s:%s when EnableFileServerAccess_ is disabled";        // VA 0x32ec012

// Byte-offset access to the raw manager, manifest and request structures.
template <typename T>
T load_at(const std::byte* base, std::size_t offset) {
    T value{};
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

template <typename T>
void store_at(std::byte* base, std::size_t offset, T value) {
    std::memcpy(base + offset, &value, sizeof value);
}

}  // namespace

// Layout (byte offsets). manager +0x08 = manifest. manifest +0x00 = records (0x20 bytes each,
// sorted by signed (type, name)), +0x30 = record count, +0x80 = chunk table (0x10 bytes each),
// +0x128 = package file table (8-byte entries). Request (0x470 bytes): +0x00 resource,
// +0x08 buffer index, +0x10 package file entry, +0x218 key string, +0x418..+0x450 chunk and
// record words, +0x458 zero, +0x460 request data, +0x468 u32 status.
std::uint64_t FillResourceFileRequest(std::byte* manager, std::uint64_t type, std::uint64_t name,
                                      std::uint64_t /* x3: never read */, void* request_data,
                                      std::uint64_t resource, std::uint64_t buffer_index,
                                      std::byte* out) {
    ClearFileRequestData(out);
    store_at<std::uint64_t>(out, 0x00, resource);
    store_at<std::uint64_t>(out, 0x08, buffer_index);

    const std::byte* const manifest = load_at<const std::byte*>(manager, 0x08);
    const std::byte* const records = load_at<const std::byte*>(manifest, 0x00);
    const auto count = load_at<std::int64_t>(manifest, 0x30);
    const auto want_type = static_cast<std::int64_t>(type);
    const auto want_name = static_cast<std::int64_t>(name);

    // Binary search over (type, name), signed comparisons, lo inclusive and hi exclusive.
    const std::byte* found = nullptr;
    std::int64_t lo = 0;
    std::int64_t hi = count;
    while (lo < hi) {
        const std::int64_t mid = lo + (hi - lo) / 2;
        const std::byte* const rec = records + mid * 0x20;
        const auto rec_type = load_at<std::int64_t>(rec, 0x00);
        const auto rec_name = load_at<std::int64_t>(rec, 0x08);
        if (rec_type < want_type || (rec_type == want_type && rec_name < want_name)) {
            lo = mid + 1;
            continue;
        }
        if (rec_type > want_type || rec_name > want_name) {
            hi = mid;
            continue;
        }
        found = rec;
        break;
    }

    if (found == nullptr) {
        const char* const type_text = String(&type);
        const char* const name_text = String(&name);
        return CreateAndAddError(kSourceFile, 0x410, kErrorCategory, kMissingFormat, type_text,
                                 name_text);
    }

    const auto chunk_index = load_at<std::uint32_t>(found, 0x10);
    const auto record_word = load_at<std::uint64_t>(found, 0x18);
    const std::byte* const chunk_table = load_at<const std::byte*>(manifest, 0x80);
    const std::byte* const chunk = chunk_table + static_cast<std::size_t>(chunk_index) * 0x10;
    const auto w0 = load_at<std::uint64_t>(chunk, 0x00);
    const auto w1 = load_at<std::uint64_t>(chunk, 0x08);
    const std::byte* const package_table = load_at<const std::byte*>(manifest, 0x128);
    const auto package_file =
        load_at<std::uint64_t>(package_table, static_cast<std::size_t>(w0 & 0xffffffffU) * 8);

    store_at<std::uint64_t>(out, 0x418, chunk_index);
    store_at<std::uint64_t>(out, 0x420, w0 >> 32);
    store_at<std::uint64_t>(out, 0x428, w1 & 0xffffffffU);
    store_at<std::uint64_t>(out, 0x430, w1 >> 32);
    store_at<std::uint64_t>(out, 0x438, load_at<std::uint32_t>(found, 0x14));
    store_at<std::uint64_t>(out, 0x440, record_word & 0xffffffffU);
    store_at<std::uint64_t>(out, 0x448, record_word & 0xffffffffU);
    store_at<std::uint64_t>(out, 0x450, record_word >> 32);
    store_at<std::uint64_t>(out, 0x10, package_file);
    SPrintF(reinterpret_cast<char*>(out + 0x218), kFormatKey, name, type);
    store_at<std::uint64_t>(out, 0x458, 0);
    store_at<void*>(out, 0x460, request_data);

    // Thread-local slot. The pthread key is created on first use (-1 means not yet); the slot is
    // then set from GetGlobal unless it already holds a value.
    bool slot_ready = false;
    std::int32_t key = g_tls_key;
    if (key == -1) {
        if (g_tls_owner == nullptr) {
            slot_ready = true;
        } else {
            key = GetTlsHandle(g_tls_owner, kTlsSymbol, 0);
            g_tls_key = key;
            slot_ready = GetTLS(key) != nullptr;
        }
    } else {
        slot_ready = GetTLS(key) != nullptr;
    }
    if (!slot_ready && g_tls_owner != nullptr) {
        void* const value = GetGlobal(g_tls_owner, kTlsSymbol, 0);
        SetTLS(key, value);
    }

    store_at<std::uint32_t>(out, 0x468, 1);
    return 0;
}

}  // namespace q15t::r15::reconstruction
