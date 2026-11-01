// NRadEngine::CJson::NavigatePath
// Quest libr15.so VA 0x10a0950; ReVault binary 576.
// ReVault raw parent 100208524, reconstruction node 100600511; grade H: every branch of the 184-instruction body checked against its disassembly, both container-step call sites checked (argument registers match the helper signatures); callees named; name confirmed by pnsrad.dll function row CJson::NavigatePath 0x18009dec0 (function_id 33972), whose callees are CJson::NavigateArrayPath 0x18009a880 and CJson::NavigateObjectPath 0x18009aad0; no libr15 symbol.
// @0x10a0950 — NRadEngine::CJson::NavigatePath
/* @confidence: H */
#include <cstddef>
#include <cstdint>
#include <cstring>

// libjansson json_t, opaque here: its first word is the json_type tag (0 = JSON_OBJECT, 1 = JSON_ARRAY).
struct json_t;

// Path cache, the fourth argument (CJson+0x08). Read here at fixed offsets only:
//   +0x00  pointer to 4096 uint32_t bucket heads, indexed by the low 12 bits of the symbol value
//   +0x18  pointer to the chunk table: one pointer per chunk, 8 bytes each
//   +0x60  uint32_t entries per chunk
// An entry is 0x20 bytes: +0x00 uint32_t index of the next entry (0xffffffff ends the chain),
// +0x10 uint64_t symbol value, +0x18 json_t* value.
struct CJsonCache;

// libjansson accessors, reached once the container of the last step is known.
extern "C" json_t* json_object_get(const json_t* object, const char* key);
extern "C" json_t* json_array_get(const json_t* array, std::size_t index);

namespace NRadEngine {

class CMemory {
 public:
  // CMemory::Fill(void*, unsigned char, unsigned long long) at Quest libr15.so VA 0x12cafcc, reached through thunk 0x1047300.
  static void Fill(void* destination, unsigned char value, unsigned long long count);
};

// CFixedStringT<1024>: the 1024-byte message buffer; SPrintF's `this` is that buffer.
template <std::size_t N>
struct CFixedStringT {
  char text[N];
  // CFixedStringT<1024>::SPrintF(char const*, ...) at Quest libr15.so VA 0x1081780, reached through thunk 0x1011a90.
  std::uint64_t SPrintF(const char* format, ...);
};

namespace NWriteLog {
enum ELogLevel : int;
// NWriteLog::WriteLog(ELogLevel, unsigned long long, char const*, ...) at Quest libr15.so VA 0x1088c20, reached through thunk 0x104b960.
void WriteLog(ELogLevel level, unsigned long long channel, const char* format, ...);
}  // namespace NWriteLog

struct CSymbol64 {
  // NRadEngine::CSymbol64::CalculateSymbolValue at Quest libr15.so VA 0x1046210. Called as
  // (path, -1, 0, -1, 1); the returned 64-bit value is the cache key. Arguments 3 to 5 are not recovered.
  static std::uint64_t CalculateSymbolValue(const char* text, std::int64_t length, std::uint32_t arg3, std::uint64_t arg4, std::uint32_t arg5);
};

class CJson {
 public:
  // NRadEngine::CJson::NavigatePath at Quest libr15.so VA 0x10a0950, called directly by the CJson getters (IsArray, Count,
  // TString, Int, ...). Name from the echovr.exe function CJson_NavigatePath (0x1405fcea0) and the pnsrad.dll function
  // CJson::NavigatePath (0x18009dec0); libr15 has no symbol carrying it.
  static const json_t* NavigatePath(const char* path, const json_t* root, int required, CJsonCache* cache);
  // NRadEngine::CJson::NavigateObjectPath at Quest libr15.so VA 0x10ad730, the object-container step; declared as in
  // cjson_navigate_object_path.cpp.
  static const json_t* NavigateObjectPath(const char* path, const json_t* container, const char* ctx_name, int required, const char** out_remaining, std::uint64_t* out_index);
  // NRadEngine::CJson::NavigateArrayPath at Quest libr15.so VA 0x10ad438, the array-container step; declared as in
  // cjson_navigate_array_path.cpp.
  static const json_t* NavigateArrayPath(const char* path, const json_t* container, const char* ctx_name, int required, const char** out_remaining, std::uint64_t* out_index);
};

}  // namespace NRadEngine

// The library's diagnostic form: separator line, message, separator line, at level 8 on channel 0.
static void LogPathMessage(const char* message) {
  using NRadEngine::NWriteLog::ELogLevel;
  NRadEngine::NWriteLog::WriteLog(static_cast<ELogLevel>(8), 0, "\n");
  NRadEngine::NWriteLog::WriteLog(static_cast<ELogLevel>(8), 0, "%s", message);
  NRadEngine::NWriteLog::WriteLog(static_cast<ELogLevel>(8), 0, "\n");
}

const json_t* NRadEngine::CJson::NavigatePath(const char* path, const json_t* root, int required, CJsonCache* cache) {
  // 0x10a0950..0x10a0964: a null path is an error, logged only when required is non-zero.
  if (path == nullptr) {
    if (required == 0) {
      return nullptr;
    }
    LogPathMessage("$ json path: invalid path.");
    return nullptr;
  }

  // 0x10a0968..0x10a09e0: skip one leading '|', or the '[' of a closed "[digits]" selector.
  const char* rest = path;
  if (*rest == '|') {
    rest += 1;
  } else if (*rest == '[') {
    std::size_t i = 1;
    for (;;) {
      const unsigned char c = static_cast<unsigned char>(rest[i]);
      if (c == '\0') {
        break;
      }
      if (c == ']') {
        rest += 1;
        break;
      }
      if (static_cast<unsigned>(c) - static_cast<unsigned>('0') > 9u) {
        break;
      }
      i += 1;
    }
  }
  // 0x10a09e0..0x10a09e4: an empty remainder is the root itself.
  if (*rest == '\0') {
    return root;
  }

  // 0x10a09e8..0x10a0a38: the root tag must be 0 (object) or 1 (array); the array tag selects the array step.
  bool root_ok = false;
  bool array_root = false;
  if (root != nullptr) {
    int tag = 0;
    std::memcpy(&tag, root, sizeof tag);
    root_ok = tag == 0 || tag == 1;
    array_root = tag == 1;
  }
  if (!root_ok) {
    if (required == 0) {
      return nullptr;
    }
    LogPathMessage("$ json path: invalid root.");
    return nullptr;
  }

  if (cache != nullptr) {
    // 0x10a0a44..0x10a0ad4: the path cache is searched by the symbol value of the remainder.
    const std::uint64_t symbol = CSymbol64::CalculateSymbolValue(rest, -1, 0, 0xffffffffffffffffULL, 1);
    const auto* bytes = reinterpret_cast<const std::byte*>(cache);
    const std::byte* buckets = nullptr;
    std::memcpy(&buckets, bytes + 0x00, sizeof buckets);
    const std::byte* chunk_table = nullptr;
    std::memcpy(&chunk_table, bytes + 0x18, sizeof chunk_table);
    std::uint32_t per_chunk = 0;
    std::memcpy(&per_chunk, bytes + 0x60, sizeof per_chunk);

    std::uint32_t entry_index = 0;
    std::memcpy(&entry_index, buckets + static_cast<std::size_t>(symbol & 0xfffu) * sizeof entry_index, sizeof entry_index);
    const json_t* value = nullptr;
    bool found = false;
    while (entry_index != 0xffffffffu) {
      // A zero per-chunk count leaves the chunk index at 0 (the decompile's guard; udiv by zero reads 0 on AArch64).
      const std::uint32_t chunk = per_chunk == 0 ? 0u : entry_index / per_chunk;
      const std::uint32_t slot = entry_index - chunk * per_chunk;
      const std::byte* chunk_base = nullptr;
      std::memcpy(&chunk_base, chunk_table + static_cast<std::size_t>(chunk) * sizeof chunk_base, sizeof chunk_base);
      const std::byte* entry = chunk_base + static_cast<std::size_t>(slot) * 0x20;
      std::uint64_t entry_symbol = 0;
      std::memcpy(&entry_symbol, entry + 0x10, sizeof entry_symbol);
      if (entry_symbol == symbol) {
        std::memcpy(&value, entry + 0x18, sizeof value);
        found = true;
        break;
      }
      std::memcpy(&entry_index, entry, sizeof entry_index);
    }
    if (!found) {
      if (required == 0) {
        return nullptr;
      }
      CFixedStringT<1024> message{};
      CMemory::Fill(message.text, 0, 0x400);
      message.SPrintF("$ json path: %s: not found in cache.", rest);
      LogPathMessage(message.text);
      return nullptr;
    }
    return value;
  }

  // 0x10a0aac..0x10a0ac8: the helper for the root's tag resolves the container of the last step, its key and its index.
  const char* key = nullptr;
  std::uint64_t index = 0;
  const json_t* container = array_root
      ? CJson::NavigateArrayPath(rest, root, "<root>", required, &key, &index)
      : CJson::NavigateObjectPath(rest, root, "<root>", required, &key, &index);
  if (container == nullptr) {
    return nullptr;
  }
  int container_tag = 0;
  std::memcpy(&container_tag, container, sizeof container_tag);

  if (container_tag == 0) {
    // 0x10a0b30..0x10a0b98: an object takes the final key.
    const json_t* found = nullptr;
    if (key != nullptr) {
      found = json_object_get(container, key);
    }
    if (required == 0 || found != nullptr) {
      return found;
    }
    CFixedStringT<1024> message{};
    CMemory::Fill(message.text, 0, 0x400);
    const char* shown = (rest != nullptr && *rest != '\0') ? rest : "<unknown>";
    message.SPrintF("$ json path: %s: { %s } not found.", shown, key);
    LogPathMessage(message.text);
    return nullptr;
  }

  // 0x10a0b10..0x10a0b54: an array takes the final index; any other tag yields no value.
  const json_t* found = nullptr;
  if (container_tag == 1) {
    found = json_array_get(container, static_cast<std::size_t>(index));
  }
  if (required == 0 || found != nullptr) {
    return found;
  }
  CFixedStringT<1024> message{};
  CMemory::Fill(message.text, 0, 0x400);
  const char* shown = (rest != nullptr && *rest != '\0') ? rest : "<unknown>";
  message.SPrintF("$ json path: index %llu not found in %s.", static_cast<unsigned long long>(index), shown);
  LogPathMessage(message.text);
  return nullptr;
}
