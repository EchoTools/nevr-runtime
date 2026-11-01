// Quest libr15.so VA 0x10a2f94; ReVault binary libr15.so.
// ReVault raw parent 100208556, reconstruction node 100600609; grade M.
#include <cstddef>
#include <cstdint>
#include <cstring>

// @0x10a2f94 — NRadEngine::CJson::SetBoolean
/* @confidence: M */

struct json_t;

extern "C" {
json_t* json_object_get(const json_t* object, const char* key);
json_t* json_array_get(const json_t* array, std::size_t index);
int json_object_del(json_t* object, const char* key);
int json_object_set_new(json_t* object, const char* key, json_t* value);
int json_array_set_new(json_t* array, std::size_t index, json_t* value);
json_t* json_true();
json_t* json_false();
}

namespace NRadEngine {
struct CAllocator;
}

extern NRadEngine::CAllocator* mspace;
extern std::int64_t mmode;

namespace NRadEngine {

struct CJsonCache;

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
struct STempString {
    unsigned char storage[N];
};

template <std::size_t N>
STempString<N> MakeTStrPrintF(const char* format, ...);

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
    void SetBoolean(const char* path, bool value);
    static json_t* FindMutableParent(const char* path, CJson* owner,
                                     const char** object_key,
                                     std::uint64_t* array_index);
};

namespace {

std::uint32_t JsonType(const json_t* value) {
    std::uint32_t type = 0;
    std::memcpy(&type, value, sizeof(type));
    return type;
}

void LogJsonPathMessage(const char* message) {
    using NWriteLog::ELogLevel;
    NWriteLog::WriteLog(static_cast<ELogLevel>(8), 0, "\n");
    NWriteLog::WriteLog(static_cast<ELogLevel>(8), 0, "%s", message);
    NWriteLog::WriteLog(static_cast<ELogLevel>(8), 0, "\n");
}

}  // namespace

void CJson::SetBoolean(const char* path, bool value) {
    const auto* const object = reinterpret_cast<const std::byte*>(this);
    CJsonCache* cache = nullptr;
    std::memcpy(&cache, object + 8, sizeof(cache));
    if (cache != nullptr) {
        const auto message = MakeTStrPrintF<512>(
            "$ json path: %s: ERROR, json db is cached, read only.", path);
        LogJsonPathMessage(reinterpret_cast<const char*>(message.storage));
        return;
    }

    CAllocator* selected_allocator = mspace;
    if (mmode != 1) {
        selected_allocator = CMemoryContext::CurrentPtr();
    }
    CAllocator* const previous_allocator = CMemoryContext::CurrentPtr();
    CMemoryContext::SetCurrent(selected_allocator);

    const char* object_key = nullptr;
    std::uint64_t array_index = 0;
    json_t* const parent =
        FindMutableParent(path, this, &object_key, &array_index);
    if (parent != nullptr) {
        if (JsonType(parent) == 0) {
            json_t* const old_value = json_object_get(parent, object_key);
            bool replaceable = old_value == nullptr;
            if (old_value != nullptr) {
                const auto old_type = JsonType(old_value);
                replaceable = old_type >= 5 && old_type <= 7;
            }
            if (replaceable) {
                json_object_del(parent, object_key);
                json_t* const new_value = value ? json_true() : json_false();
                json_object_set_new(parent, object_key, new_value);
            } else {
                CFixedStringT<1024> message{};
                CMemory::Fill(message.text, 0, 0x400);
                message.SPrintF(
                    "$ json path: cannot change type of [%s] to boolean",
                    path);
                LogJsonPathMessage(message.text);
            }
        } else {
            json_t* const old_value = json_array_get(parent, array_index);
            bool replaceable = false;
            if (old_value != nullptr) {
                const auto old_type = JsonType(old_value);
                replaceable = old_type >= 5 && old_type <= 7;
            }
            if (replaceable) {
                json_t* const new_value = value ? json_true() : json_false();
                json_array_set_new(parent, array_index, new_value);
            } else {
                CFixedStringT<1024> message{};
                CMemory::Fill(message.text, 0, 0x400);
                message.SPrintF(
                    "$ json path: cannot change type of [%s] to boolean",
                    path);
                LogJsonPathMessage(message.text);
            }
        }
    }

    CMemoryContext::SetCurrent(previous_allocator);
}

}  // namespace NRadEngine
