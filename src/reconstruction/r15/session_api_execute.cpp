// NRadEngine::NRadGame::NR15RestAPI::SAPIWrapper<SSessionRequest>::Execute
// Quest libr15.so VA 0x13fe8c4; ReVault binary 576.
// ReVault raw parent 100226344, reconstruction node 100600482; grade M: all 147 instructions and every branch are mapped, but the SSessionRequest::Execute call sets neither x1 nor x3 on its path and the response ctor is placed once above the branches.
// @0x13fe8c4 — NRadEngine::NRadGame::NR15RestAPI::SAPIWrapper<SSessionRequest>::Execute
/* @confidence: M */
#include <cstddef>
#include <cstdint>
#include <cstring>

// Session error descriptions; ReVault VA 0x37318a8 is entry 0. The listing indexes entries 2, 5 and 6 with -code.
extern const char* const kSessionErrorText[7];

namespace NRadEngine {

class CAllocator;

// Text buffer. CMemBlock derives from it: the listing calls operator+= with the block as this.
class CString {
 public:
  CString& operator+=(const char* text);      // 0x100b8c0 (CString::operator+=(char*))
  CString& operator+=(const CString& other);  // 0x103ca70 (CString::operator+=(CString*))
};

// Memory block whose flags byte at +0x1c decides whether Detach runs before the destructor.
class CMemBlock : public CString {
 public:
  CMemBlock(std::uint64_t initialSize, std::uint64_t reserve, CAllocator* allocator);  // 0xfedec0
  ~CMemBlock();                                                                        // 0x100e200
  void Detach();                                                                       // 0xfe2470
  alignas(8) std::byte raw[0x38];
};
static_assert(sizeof(CMemBlock) == 0x38);

class CMemoryContext {
 public:
  static CAllocator* CurrentPtr();  // 0xffbc50
};

class CJson {
 public:
  CJson();                                             // 0xfd7760 (CJson::CJson, 0x10a2250)
  ~CJson();                                            // 0xfdcae0 (CJson::~CJson, 0x10a2258)
  void SetInt(const char* key, std::int64_t value);    // 0x1028210 (CJson::SetInt, 0x10a5edc)
  void SetString(const char* key, const char* value);  // 0x103a7c0 (CJson::SetString, 0x10a3444)
};

int ExtractErrorMessage(int bodyResult, std::uint32_t includeOffset, std::uint32_t includeContext,
                        CString* output);  // 0x10494c0

class CHttpListenerRequest {
 public:
  // The listing tests the int in w0 after Body; Ghidra's thunk shows void.
  int Body(CJson* body);                                      // 0x1007180 (CHttpListenerRequest::Body, 0x10940b4)
  void ReplyWithJson(CJson* response, std::uint32_t status);  // 0x1047460 (CHttpListenerRequest::ReplyWithJson, 0x1094154)
};

namespace NRadGame {
namespace NR15RestAPI {

struct SR15APIContext;  // the five-word block forwarded to SSessionRequest::Execute (sp+0x90 in the listing)

class SSessionRequest {
 public:
  static void Execute(SR15APIContext* words, CJson* body, CJson* response, std::int64_t* extra);  // 0x13b76d4 via thunk 0x1026720
};

template <typename TRequest>
class SAPIWrapper : public CHttpListenerRequest {
 public:
  void Execute(void* context);
};

// Game type hashes compared by the listing; the words at 0x2ca4b30, 0x2cace80 and 0x2cacea0 hold these values.
constexpr std::uint64_t kRestrictedGameTypeHashes[] = {
    0x042d9cf9cfddcf76ULL,
    0x305d6e37c1589c45ULL,
    0x163956e6e431e617ULL,
};

using TypeHashSlotFn = std::uint64_t (*)(const std::byte* self);

// Reads the 64-bit word at base+offset.
static std::uintptr_t LoadWord(const std::byte* base, std::size_t offset) {
  std::uintptr_t value = 0;
  std::memcpy(&value, base + offset, sizeof value);
  return value;
}

// Writes a 64-bit field of a CMemBlock that the listing stores after the constructor.
static void StoreWord(std::byte* base, std::size_t offset, std::uintptr_t value) {
  std::memcpy(base + offset, &value, sizeof value);
}

// The flags byte at +0x1c of a CMemBlock.
static std::uint8_t LoadFlags(const CMemBlock& block) {
  std::uint8_t flags = 0;
  std::memcpy(&flags, reinterpret_cast<const std::byte*>(&block) + 0x1c, sizeof flags);
  return flags;
}

// Calls virtual slot 5 (+0x28) of the game object; the type hash comes back in x0.
static std::uint64_t CallTypeHashSlot(const std::byte* game) {
  const auto* vtable = reinterpret_cast<const std::byte*>(LoadWord(game, 0));
  const auto slot = reinterpret_cast<TypeHashSlotFn>(LoadWord(vtable, 0x28));
  return slot(game);
}

template <>
void SAPIWrapper<SSessionRequest>::Execute(void* context) {
  CAllocator* allocator = CMemoryContext::CurrentPtr();  // 0xffbc50
  CMemBlock errorMessage(0, 0, allocator);                // 0xfedec0
  std::byte* errorBytes = reinterpret_cast<std::byte*>(&errorMessage);
  StoreWord(errorBytes, 0x20, 0x20);
  StoreWord(errorBytes, 0x28, 0);
  StoreWord(errorBytes, 0x30, 0);

  const auto* contextBytes = static_cast<const std::byte*>(context);
  const auto* api = reinterpret_cast<const std::byte*>(LoadWord(contextBytes, 0x08));
  // Forwarded block: ctx+0x2b08, ctx+0x2b10, api, ctx, ctx+0x2aa8.
  std::uintptr_t words[5] = {
      LoadWord(contextBytes, 0x2b08),
      LoadWord(contextBytes, 0x2b10),
      reinterpret_cast<std::uintptr_t>(api),
      reinterpret_cast<std::uintptr_t>(context),
      LoadWord(contextBytes, 0x2aa8),
  };

  // Feature bit 3 of the byte at api+0x8c09; clear means the error path with code -2.
  std::uint8_t featureByte = 0;
  std::memcpy(&featureByte, api + 0x8c09, sizeof featureByte);
  std::int64_t code = 0;  // -2 feature off, -5 not kind 9, -6 restricted game type, 0 delegate
  if (((featureByte >> 3) & 1) == 0) {
    code = -2;
  } else {
    std::int32_t requestKind = 0;
    std::memcpy(&requestKind, contextBytes, sizeof requestKind);
    if (requestKind != 9) {
      code = -5;
    } else {
      const auto* game = reinterpret_cast<const std::byte*>(LoadWord(contextBytes, 0x40));
      bool restricted = false;
      if (game != nullptr) {
        const std::uint64_t gameType = CallTypeHashSlot(game);
        for (const std::uint64_t restrictedType : kRestrictedGameTypeHashes) {
          restricted = restricted || gameType == restrictedType;
        }
      }
      code = restricted ? -6 : 0;
    }
  }

  {
    CJson response;  // ctor call sites: 0x13fe92c, 0x13fe9e0, 0x13fe9f4, 0x13feb04
    std::uint32_t status = 0;
    if (code != 0) {
      response.SetInt("err_code", code);                                // 0x1028210
      response.SetString("err_description", kSessionErrorText[-code]);  // 0x103a7c0
      status = 500;
    } else {
      {
        CJson body;                                // ctor call at 0x13fea00 (sp+0x38)
        const int bodyResult = this->Body(&body);  // 0x1007180
        if (bodyResult == 0) {
          // The listing sets neither x1 nor x3 on this path; body is passed as x1 and x3 as null.
          SSessionRequest::Execute(reinterpret_cast<SR15APIContext*>(words), &body, &response,
                                   nullptr);  // 0x1026720
        } else {
          CMemBlock parseText(0, 0, CMemoryContext::CurrentPtr());  // 0xffbc50, 0xfedec0
          std::byte* parseBytes = reinterpret_cast<std::byte*>(&parseText);
          StoreWord(parseBytes, 0x20, 0x20);
          StoreWord(parseBytes, 0x28, 0);
          StoreWord(parseBytes, 0x30, 0);
          if (ExtractErrorMessage(bodyResult, 1u, 1u, &parseText) != 0) {  // 0x10494c0
            errorMessage += "Parsing error on json body parameters: ";    // 0x100b8c0
            errorMessage += parseText;                                    // 0x103ca70
          }
          if ((LoadFlags(parseText) & 6) != 0) {
            parseText.Detach();  // 0xfe2470
          }
        }  // ~CMemBlock(parseText): 0x100e200
      }    // ~CJson(body): 0xfdcae0
      response.SetInt("err_code", 0);  // 0x1028210 with value 0
      status = 200;
    }
    this->ReplyWithJson(&response, status);  // 0x1047460
  }                                          // ~CJson(response): 0xfdcae0
  if ((LoadFlags(errorMessage) & 6) != 0) {
    errorMessage.Detach();  // 0xfe2470
  }
}  // ~CMemBlock(errorMessage): 0x100e200

}  // namespace NR15RestAPI
}  // namespace NRadGame
}  // namespace NRadEngine
