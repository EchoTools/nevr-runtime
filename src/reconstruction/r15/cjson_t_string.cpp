// NRadEngine::CJson::TString
// Quest libr15.so VA 0x10a2e7c; ReVault binary 576.
// ReVault raw parent 100208555, reconstruction node 100600493; grade H: all 70 instructions read; the found-string path and both miss paths (flag 0, flag set) match the listing; stored size 280 is 70 instructions.
// @0x10a2e7c — NRadEngine::CJson::TString
/* @confidence: H */
#include <cstddef>
#include <cstdint>
#include <cstring>

// jansson json_t, opaque here: its first word is the type tag (2 = JSON_STRING).
struct json_t;
struct CJsonCache;

// Quest libr15.so VA 0x12c7764, reached through thunk 0x1042d80: jansson json_string_value.
extern "C" const char* json_string_value(const json_t* json);

namespace NRadEngine {

struct CAllocator;

}  // namespace NRadEngine

// Ghidra names mspace (its address is in the GOT slot at 0x3829e08) and mmode (GOT slot 0x380d098).
extern NRadEngine::CAllocator* mspace;
extern std::int64_t mmode;

namespace NRadEngine {

struct CMemoryContext {
  // CMemoryContext::CurrentPtr() at Quest libr15.so VA 0x12cae90, reached through thunk 0xffbc50.
  // The callers use the result as a CAllocator*.
  static CAllocator* CurrentPtr();
  // CMemoryContext::SetCurrent(CAllocator*) at Quest libr15.so VA 0x12cafac, reached through thunk 0xfd3590.
  static void SetCurrent(CAllocator* allocator);
};

class CMemory {
 public:
  // CMemory::Fill(void*, unsigned char, unsigned long long) at Quest libr15.so VA 0x12cafcc, reached through
  // thunk 0x1047300; it tail-calls memset (thunk 0xfe71f0).
  static void Fill(void* destination, unsigned char value, unsigned long long count);
};

// CFixedStringT<1024>: the 1024-byte buffer the caller keeps on its stack; SPrintF's `this` is that buffer.
template <std::size_t N>
struct CFixedStringT {
  char text[N];
  // CFixedStringT<1024>::SPrintF(char const*, ...) at Quest libr15.so VA 0x1081780, reached through thunk 0x1011a90.
  std::uint64_t SPrintF(const char* format, ...);
};

namespace NWriteLog {
enum ELogLevel : int;
// NWriteLog::WriteLog(ELogLevel, unsigned long long, char const*, ...) at Quest libr15.so VA 0x1088c20,
// reached through thunk 0x104b960.
void WriteLog(ELogLevel level, unsigned long long channel, const char* format, ...);
}  // namespace NWriteLog

class CJson {
 public:
  // NRadEngine::CJson::NavigatePath at Quest libr15.so VA 0x10a0950, called by TString (0x10a2e7c). The name comes from
  // the echovr.exe function CJson_NavigatePath (0x1405fcea0); libr15 has no name for it.
  static const json_t* NavigatePath(const char* path, const json_t* root, int required, CJsonCache* cache);
  // NRadEngine::CJson::TString(char const*, char const*, unsigned int) const at Quest libr15.so VA 0x10a2e7c,
  // reached through thunk 0x1028e40 (adrp, ldr, br). The object is two words: json_t* root at +0x00 and a
  // cache pointer at +0x08 (CJson::CJson() at Quest libr15.so VA 0x10a2250 zeroes both).
  const char* TString(const char* key, const char* fallback, std::uint32_t flag) const;
};

}  // namespace NRadEngine

const char* NRadEngine::CJson::TString(const char* key, const char* fallback, std::uint32_t flag) const {
  const auto* object = reinterpret_cast<const std::byte*>(this);

  // 0x10a2e90..0x10a2ec0: the lookup allocator is mspace when mmode == 1, otherwise the current allocator.
  CAllocator* lookup_allocator = mspace;
  if (mmode != 1) {
    lookup_allocator = CMemoryContext::CurrentPtr();
  }
  // 0x10a2ecc..0x10a2ed8: remember the current allocator, then make the lookup allocator current.
  CAllocator* previous_allocator = CMemoryContext::CurrentPtr();
  CMemoryContext::SetCurrent(lookup_allocator);

  // 0x10a2edc..0x10a2ee8: the path lookup gets (key, word 0, flag, word 1) of this object.
  const json_t* root = nullptr;
  std::memcpy(&root, object, sizeof(root));
  CJsonCache* cache = nullptr;
  std::memcpy(&cache, object + 8, sizeof(cache));
  const json_t* value = CJson::NavigatePath(key, root, static_cast<int>(flag), cache);

  // 0x10a2eec..0x10a2ef8: the value counts only when its type word is 2 (JSON_STRING).
  bool is_string = false;
  if (value != nullptr) {
    std::int32_t type = 0;
    std::memcpy(&type, value, sizeof(type));
    is_string = type == 2;
  }

  const char* result = fallback;
  if (is_string) {
    // 0x10a2efc..0x10a2f00: json_string_value(value) is the result.
    result = json_string_value(value);
  } else if (flag != 0) {
    // 0x10a2f08..0x10a2f6c: with flag set, a miss logs: the 1024-byte message, then three lines at level 8.
    CFixedStringT<1024> message;
    CMemory::Fill(message.text, 0, 0x400);
    message.SPrintF("$ json path: [%s] not found or not a string.", key);
    NWriteLog::WriteLog(static_cast<NWriteLog::ELogLevel>(8), 0, "\n");
    NWriteLog::WriteLog(static_cast<NWriteLog::ELogLevel>(8), 0, "%s", message.text);
    NWriteLog::WriteLog(static_cast<NWriteLog::ELogLevel>(8), 0, "\n");
  }

  // 0x10a2f70..0x10a2f78: restore the previous allocator; the result is returned in x0.
  CMemoryContext::SetCurrent(previous_allocator);
  return result;
}
