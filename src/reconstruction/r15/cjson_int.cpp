// NRadEngine::CJson::Int
// Quest libr15.so VA 0x10a40ec; ReVault binary 576.
// ReVault raw parent 100208580, reconstruction node 100600498; grade H: all 70 instructions read; the found-integer path and both miss paths (flag 0, flag set) match the listing; stored size 280 is 70 instructions.
// @0x10a40ec — NRadEngine::CJson::Int
/* @confidence: H */
#include <cstddef>
#include <cstdint>
#include <cstring>

// jansson json_t, opaque here: its first word is the type tag (3 = JSON_INTEGER).
struct json_t;

// Path cache, the fourth argument of NavigatePath (CJson+0x08). NavigatePath reads it at fixed offsets only:
//   +0x00  pointer to 4096 uint32_t bucket heads, indexed by the low 12 bits of the symbol value
//   +0x18  pointer to the chunk table: one pointer per chunk, 8 bytes each
//   +0x60  uint32_t entries per chunk
// An entry is 0x20 bytes: +0x00 uint32_t index of the next entry (0xffffffff ends the chain),
// +0x10 uint64_t symbol value, +0x18 json_t* value.
struct CJsonCache;

// Quest libr15.so VA 0x12c79a0, reached through thunk 0x1029950: jansson json_integer_value.
extern "C" std::int64_t json_integer_value(const json_t* json);

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
  // NRadEngine::CJson::NavigatePath at Quest libr15.so VA 0x10a0950, called directly from Int at 0x10a4158.
  // Name from the echovr.exe function CJson_NavigatePath (0x1405fcea0); libr15 has no symbol carrying it.
  static const json_t* NavigatePath(const char* path, const json_t* root, int required, CJsonCache* cache);

  // NRadEngine::CJson::Int(char const*, long long, unsigned int) const at Quest libr15.so VA 0x10a40ec,
  // reached through thunk 0x10152d0 (adrp, ldr, br). The object is two words: json_t* root at +0x00 and a
  // cache pointer at +0x08 (CJson::CJson() at Quest libr15.so VA 0x10a2250 zeroes both).
  std::int64_t Int(const char* key, std::int64_t fallback, std::uint32_t flag) const;
};

}  // namespace NRadEngine

std::int64_t NRadEngine::CJson::Int(const char* key, std::int64_t fallback, std::uint32_t flag) const {
  const auto* object = reinterpret_cast<const std::byte*>(this);

  // 0x10a4100..0x10a4124: the lookup allocator is mspace when mmode == 1, otherwise the current allocator.
  CAllocator* lookup_allocator = mspace;
  if (mmode != 1) {
    lookup_allocator = CMemoryContext::CurrentPtr();
  }
  // 0x10a413c..0x10a4148: remember the current allocator, then make the lookup allocator current.
  CAllocator* previous_allocator = CMemoryContext::CurrentPtr();
  CMemoryContext::SetCurrent(lookup_allocator);

  // 0x10a414c..0x10a4158: the path lookup gets (key, word 0, flag, word 1) of this object.
  const json_t* root = nullptr;
  std::memcpy(&root, object, sizeof(root));
  CJsonCache* cache = nullptr;
  std::memcpy(&cache, object + 8, sizeof(cache));
  const json_t* value = NavigatePath(key, root, static_cast<int>(flag), cache);

  // 0x10a415c..0x10a4168: the value counts only when its type word is 3 (JSON_INTEGER).
  bool is_integer = false;
  if (value != nullptr) {
    std::int32_t type = 0;
    std::memcpy(&type, value, sizeof(type));
    is_integer = type == 3;
  }

  std::int64_t result = fallback;
  if (is_integer) {
    // 0x10a416c..0x10a4170: json_integer_value(value) is the result.
    result = json_integer_value(value);
  } else if (flag != 0) {
    // 0x10a4178..0x10a41d8: with flag set, a miss logs: the 1024-byte message, then three lines at level 8.
    CFixedStringT<1024> message;
    CMemory::Fill(message.text, 0, 0x400);
    message.SPrintF("$ json path: [%s] not found or not an integer.", key);
    NWriteLog::WriteLog(static_cast<NWriteLog::ELogLevel>(8), 0, "\n");
    NWriteLog::WriteLog(static_cast<NWriteLog::ELogLevel>(8), 0, "%s", message.text);
    NWriteLog::WriteLog(static_cast<NWriteLog::ELogLevel>(8), 0, "\n");
  }

  // 0x10a41e0..0x10a41e8: restore the previous allocator; the result is returned in x0.
  CMemoryContext::SetCurrent(previous_allocator);
  return result;
}
