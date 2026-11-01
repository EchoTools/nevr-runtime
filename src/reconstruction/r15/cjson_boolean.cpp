// Quest libr15.so VA 0x10a4370; ReVault binary libr15.so.
// ReVault raw parent 100208583, reconstruction node 100600613; grade M.
#include <cstddef>
#include <cstdint>
#include <cstring>

// @0x10a4370 — NRadEngine::CJson::Boolean
/* @confidence: M */

struct json_t;

namespace NRadEngine {
struct CAllocator;
struct CJsonCache;
}

extern NRadEngine::CAllocator* mspace;
extern std::int64_t mmode;

namespace NRadEngine {

struct CMemoryContext {
    static CAllocator* CurrentPtr();
    static void SetCurrent(CAllocator* allocator);
};

class CMemory {
public:
    static void Fill(void* destination, unsigned char value,
                     std::uint64_t count);
};

template <std::size_t N>
struct CFixedStringT {
    char text[N];
    std::uint64_t SPrintF(const char* format, ...);
};

namespace NWriteLog {
enum ELogLevel : int;
void WriteLog(ELogLevel level, std::uint64_t channel, const char* format, ...);
}  // namespace NWriteLog

class CJson {
public:
    std::uint32_t Boolean(const char* path, std::uint32_t fallback,
                          std::uint32_t required) const;
    static const json_t* NavigatePath(const char* path, const json_t* root,
                                      int required, CJsonCache* cache);
};

std::uint32_t CJson::Boolean(const char* path, std::uint32_t fallback,
                             std::uint32_t required) const {
    const auto* const object = reinterpret_cast<const std::byte*>(this);
    const json_t* root = nullptr;
    CJsonCache* cache = nullptr;
    std::memcpy(&root, object, sizeof(root));
    std::memcpy(&cache, object + 8, sizeof(cache));

    CAllocator* selected_allocator = mspace;
    if (mmode != 1) {
        selected_allocator = CMemoryContext::CurrentPtr();
    }
    CAllocator* const previous_allocator = CMemoryContext::CurrentPtr();
    CMemoryContext::SetCurrent(selected_allocator);

    const json_t* const value =
        NavigatePath(path, root, static_cast<int>(required), cache);
    std::uint32_t result = fallback;
    std::uint32_t type = 0;
    if (value != nullptr) {
        std::memcpy(&type, value, sizeof(type));
    }
    if (value == nullptr || (type != 5 && type != 6)) {
        if (required != 0) {
            CFixedStringT<1024> message{};
            CMemory::Fill(message.text, 0, 0x400);
            message.SPrintF(
                "$ json path: [%s] not found or not a boolean.", path);
            using NWriteLog::ELogLevel;
            NWriteLog::WriteLog(static_cast<ELogLevel>(8), 0, "\n");
            NWriteLog::WriteLog(static_cast<ELogLevel>(8), 0, "%s",
                                message.text);
            NWriteLog::WriteLog(static_cast<ELogLevel>(8), 0, "\n");
        }
    } else {
        result = type == 5 ? 1U : 0U;
    }

    CMemoryContext::SetCurrent(previous_allocator);
    return result;
}

}  // namespace NRadEngine
