// NRadEngine::CJson::NavigateObjectPath
// Quest libr15.so VA 0x10ad730; ReVault binary 576.
// ReVault raw parent 100208733, reconstruction node 100600534; grade H: every branch of the 208-instruction body checked against its disassembly; names cross-matched to pnsrad CJson::NavigateObjectPath; compiled clean under -Wall -Wextra -Werror (mingw g++ 16.2, -O3).
// @0x10ad730 — NRadEngine::CJson::NavigateObjectPath
/* @confidence: H */
#include <cstddef>
#include <cstdint>
#include <cstring>

// libjansson json_t, opaque here: its first word is the json_type tag (0 = JSON_OBJECT, 1 = JSON_ARRAY).
struct json_t;

// libjansson json_object_get: reached through thunk 0x100eea0, the function at Quest libr15.so VA 0x12c6ab8.
extern "C" json_t* json_object_get(const json_t* object, const char* key);

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

class CJson {
 public:
  // NRadEngine::CJson::NavigateArrayPath at Quest libr15.so VA 0x10ad438, the array-container step; called from this function at 0x10ada64.
  static const json_t* NavigateArrayPath(const char* path, const json_t* container, const char* ctx_name, int required, const char** out_remaining, std::uint64_t* out_index);
  // NRadEngine::CJson::NavigateObjectPath at Quest libr15.so VA 0x10ad730 (this function); recursion at 0x10ad978.
  static const json_t* NavigateObjectPath(const char* path, const json_t* container, const char* ctx_name, int required, const char** out_remaining, std::uint64_t* out_index);
};

}  // namespace NRadEngine

// The library's diagnostic form: separator line, message, separator line, at level 8 on channel 0 (the "\n" string at Quest libr15.so VA 0x2c3f20f).
static void LogPathMessage(const char* message) {
  using NRadEngine::NWriteLog::ELogLevel;
  NRadEngine::NWriteLog::WriteLog(static_cast<ELogLevel>(8), 0, "\n");
  NRadEngine::NWriteLog::WriteLog(static_cast<ELogLevel>(8), 0, "%s", message);
  NRadEngine::NWriteLog::WriteLog(static_cast<ELogLevel>(8), 0, "\n");
}

// The JSON tag of a json_t: its first word (0 = object, 1 = array).
static int TypeTag(const json_t* node) {
  int tag = 0;
  std::memcpy(&tag, node, sizeof tag);
  return tag;
}

// The context name shown in a message: the name, or "<unknown>" when it is null or empty (string at Quest libr15.so VA 0x2c2efd0).
static const char* ShownName(const char* name) {
  return (name != nullptr && *name != '\0') ? name : "<unknown>";
}

// Whether the '[' at `open` starts a closed "[digits]" selector: digits up to a ']'.
static bool IsClosedIndex(const char* open) {
  for (std::size_t i = 1;; ++i) {
    const unsigned char c = static_cast<unsigned char>(open[i]);
    if (c == '\0') {
      return false;
    }
    if (c == ']') {
      return true;
    }
    if (static_cast<unsigned>(c) - static_cast<unsigned>('0') >= 10u) {
      return false;
    }
  }
}

const json_t* NRadEngine::CJson::NavigateObjectPath(const char* path, const json_t* container, const char* ctx_name, int required, const char** out_remaining, std::uint64_t* out_index) {
  // 0x10ad750..0x10ad7dc: the container must be a non-null object (tag 0); otherwise the step fails.
  if (container == nullptr || TypeTag(container) != 0) {
    // 0x10ad760..0x10ad7d8: a required step logs "is not an object" through the 1024-byte message buffer.
    if (required != 0) {
      CFixedStringT<1024> message{};
      CMemory::Fill(message.text, 0, 0x400);
      message.SPrintF("$ json path: %s is not an object.", ShownName(ctx_name));
      LogPathMessage(message.text);
    }
    return nullptr;
  }

  // 0x10ad800..0x10ad87c: the key buffer is zeroed, then the key is copied up to a '|', a closed "[digits]" or the end.
  char key[1024];
  CMemory::Fill(key, 0, 0x400);
  std::size_t key_len = 0;
  const char* cursor = path;
  for (;;) {
    const unsigned char c = static_cast<unsigned char>(*cursor);
    if (c == '[') {
      if (IsClosedIndex(cursor)) {
        break;
      }
    } else if (c == '\0' || c == '|') {
      break;
    }
    key[key_len] = static_cast<char>(c);
    key_len += 1;
    cursor += 1;
  }
  key[key_len] = '\0';

  // 0x10ad87c..0x10ad8c4 and 0x10ad998..0x10ada48: a '|' or a closed "[digits]" after the key descends to the child.
  const unsigned char stop = static_cast<unsigned char>(*cursor);
  if (stop == '|' || stop == '[') {
    // 0x10ad8c8 and 0x10ad998: the remaining path starts after the '|', or at the digits after the '['.
    *out_remaining = cursor + 1;
    // 0x10ad8cc..0x10ad988 and 0x10ad9a0..0x10ad9bc: the key is looked up in the object; its tag is tested again here.
    const json_t* child = nullptr;
    if (TypeTag(container) == 0) {
      child = json_object_get(container, key);
    }
    // 0x10ad8d8..0x10ad9cc: a required lookup that finds nothing logs "{ key } not found".
    if (required != 0 && child == nullptr) {
      CFixedStringT<1024> message{};
      CMemory::Fill(message.text, 0, 0x400);
      message.SPrintF("$ json path: %s: { %s } not found.", ShownName(ctx_name), key);
      LogPathMessage(message.text);
    }
    if (child == nullptr) {
      return nullptr;
    }
    // 0x10ad960..0x10ad97c ('|') and 0x10ada4c..0x10ada64 ('['): the child is the container of the rest of the path.
    if (stop == '|') {
      return NavigateObjectPath(*out_remaining, child, key, required, out_remaining, out_index);
    }
    return NavigateArrayPath(*out_remaining, child, key, required, out_remaining, out_index);
  }

  // 0x10ad8bc..0x10ad8c0: the path ends at this key; the container is returned and the path is the entry text.
  *out_remaining = path;
  return container;
}
