// NRadEngine::CJson::NavigateArrayPath
// Quest libr15.so VA 0x10ad438; ReVault binary 576.
// ReVault raw parent 100208732, reconstruction node 100600535; grade H: every branch of the 190-instruction body checked against its disassembly; names cross-matched to pnsrad CJson::NavigateArrayPath; compiled clean under -Wall -Wextra -Werror (mingw g++ 16.2, -O3).
// @0x10ad438 — NRadEngine::CJson::NavigateArrayPath
/* @confidence: H */
#include <cstddef>
#include <cstdint>
#include <cstring>

// libjansson json_t, opaque here: its first word is the json_type tag (0 = JSON_OBJECT, 1 = JSON_ARRAY).
struct json_t;

// libjansson json_array_get: reached through thunk 0xfdcc60, the function at Quest libr15.so VA 0x12c7068.
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

class CJson {
 public:
  // NRadEngine::CJson::ParseArrayIndex at Quest libr15.so VA 0x10ada70 (not reconstructed here). It parses "N]" from the path,
  // stores N in *index (all ones when nothing parsed) and *type (5: "[" follows, 6: "|" follows, 0: the path ends), and returns
  // the text after the selector.
  static const char* ParseArrayIndex(const char* path, std::uint64_t* index, std::int32_t* type);
  // NRadEngine::CJson::NavigateObjectPath at Quest libr15.so VA 0x10ad730; called from NavigateArrayPath at 0x10ad674.
  static const json_t* NavigateObjectPath(const char* path, const json_t* container, const char* ctx_name, int required, const char** out_remaining, std::uint64_t* out_index);
  // NRadEngine::CJson::NavigateArrayPath at Quest libr15.so VA 0x10ad438 (this function); recursion at 0x10ad724.
  static const json_t* NavigateArrayPath(const char* path, const json_t* container, const char* ctx_name, int required, const char** out_remaining, std::uint64_t* out_index);
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

// The index slot a failed ParseArrayIndex leaves behind (all ones, the -1 of the decompile).
constexpr std::uint64_t kNoIndex = ~std::uint64_t{0};

const json_t* NRadEngine::CJson::NavigateArrayPath(const char* path, const json_t* container, const char* ctx_name, int required, const char** out_remaining, std::uint64_t* out_index) {
  // 0x10ad450..0x10ad4cc: the container must be a non-null array (tag 1); otherwise the step fails.
  if (container == nullptr || TypeTag(container) != 1) {
    // 0x10ad4cc..0x10ad4f8: a required step logs "is not an array" through the 1024-byte message buffer.
    if (required != 0) {
      CFixedStringT<1024> message{};
      CMemory::Fill(message.text, 0, 0x400);
      message.SPrintF("$ json path: %s is not an array.", ShownName(ctx_name));
      LogPathMessage(message.text);
    }
    return nullptr;
  }

  // 0x10ad46c..0x10ad484: the index step parses "N]" from the path; the text after the selector goes to *out_remaining.
  std::int32_t kind = 0;
  *out_remaining = CJson::ParseArrayIndex(path, out_index, &kind);

  // 0x10ad488..0x10ad56c: an index that did not parse (the slot still all ones) is an error, "invalid index for array".
  if (*out_index == kNoIndex) {
    if (required != 0) {
      CFixedStringT<1024> message{};
      CMemory::Fill(message.text, 0, 0x400);
      message.SPrintF("$ json path: invalid index for array %s.", ShownName(ctx_name));
      LogPathMessage(message.text);
    }
    return nullptr;
  }
  const std::uint64_t index = *out_index;

  // 0x10ad494..0x10ad5c8: kind 5 ("[" follows) descends into the element as an array, kind 6 ("|" follows) as an object.
  if (kind == 5 || kind == 6) {
    // 0x10ad4b4 and 0x10ad5b8: the element is json_array_get(container, index); only a tag-1 container has elements.
    const json_t* child = nullptr;
    if (TypeTag(container) == 1) {
      child = json_array_get(container, static_cast<std::size_t>(index));
    }
    // 0x10ad4c4 and 0x10ad5c0: a required lookup that finds nothing logs "index N not found in".
    if (required != 0 && child == nullptr) {
      CFixedStringT<1024> message{};
      CMemory::Fill(message.text, 0, 0x400);
      message.SPrintF("$ json path: index %llu not found in %s.", static_cast<unsigned long long>(index), ShownName(ctx_name));
      LogPathMessage(message.text);
    }
    if (child == nullptr) {
      return nullptr;
    }
    // 0x10ad65c..0x10ad674 (kind 6) and 0x10ad70c..0x10ad724 (kind 5): the child is the container of the rest of the path.
    if (kind == 5) {
      return NavigateArrayPath(*out_remaining, child, ctx_name, required, out_remaining, out_index);
    }
    return NavigateObjectPath(*out_remaining, child, ctx_name, required, out_remaining, out_index);
  }

  // 0x10ad5c8..0x10ad5cc: any other kind ends the path; the container is returned and the path is the entry text.
  *out_remaining = path;
  return container;
}
