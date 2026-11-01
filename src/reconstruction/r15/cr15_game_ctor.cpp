// CR15Game::CR15Game
// Quest libr15.so VA 0x12fce34; ReVault binary 576.
// ReVault raw parent 100221436, reconstruction node 100600475; grade M: every block is translated from the file bytes, but the loader-filled slots and pointers (0x3816230, 0x382adc8, 0x386ce10, 0x3824368, 0x386c560), the strlen stub name and the ValidateTag name are not verifiable from the file.
// @0x12fce34 — CR15Game::CR15Game
/* @confidence: M */
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine::NRadGame {

// The constructor body at VA 0x12fce34. It is a member function named Construct because the base
// constructor and every other callee are separate functions here (declared below, each with its
// address), and a C++ constructor cannot be invoked on storage it did not create. Raw layout, none of
// it recovered as fields: +0x00 vptr, +0x7ad8, +0x7b00..+0x9050; every access is a raw byte offset
// from this object.
class CR15Game {
 public:
  void Construct(const std::byte* args);
};

}  // namespace NRadEngine::NRadGame

namespace {

// All-ones word: the -1 that the disassembly stores (x22/x23 = -1).
constexpr std::uint64_t kAllOnes = ~std::uint64_t{0};

// 16 bytes stored as two 64-bit words, low word first (little-endian image bytes).
struct Quad {
  std::uint64_t lo;
  std::uint64_t hi;
};

// Literal pools read from the image (the values, in file order).
// 0x2c9fe40: the current SR15GameSettings default {0, 0x1003, 1, 0}, stored at self+0x8c04.
constexpr Quad kSettingsCurrent{0x0000100300000000ULL, 0x0000000000000001ULL};
// 0x2c9fe60: the pending SR15GameSettings default {0x1003, 1, 0, 0xa}, stored at self+0x8ce4.
constexpr Quad kSettingsPending{0x0000000100001003ULL, 0x0000000a00000000ULL};
// 0x2c9fe50: four floats {5.0, 2.0, 17.5, 7.5}, stored at self+0x8c24.
constexpr Quad kFloatsQ2{0x4000000040a00000ULL, 0x40f00000418c0000ULL};
// 0x2c9fe20: {1.0f, 0, 0, 0}, stored at self+0x8f8c and self+0x9054.
constexpr Quad kQ1{0x000000003f800000ULL, 0x0000000000000000ULL};
// 0x2c9fe70: {0, 0x5ce71c2b}, stored at self+0x7e88.
constexpr Quad kQ0{0x0000000000000000ULL, 0x000000005ce71c2bULL};
// movi v0.4S, #0xa: four 32-bit lanes of 10.
constexpr Quad kFourTens{0x0000000a0000000aULL, 0x0000000a0000000aULL};
// movi v0.2D, #0 and the zero pair.
constexpr Quad kZero16{0, 0};
// 0x2c2f960: {-1, 0}, stored at self+0x8548.
constexpr Quad kMinusOneLow{kAllOnes, 0};

// Scalar literals loaded through d registers (bit patterns of the 8-byte image words).
constexpr std::uint64_t kD8FromPool2c76068 = 0x0000000200000000ULL;  // 0x2c76068
constexpr std::uint64_t kD0FromPool2c9fd88 = 0x0000000200000003ULL;  // 0x2c9fd88 (also the pair at self+0x8d28)
constexpr std::uint64_t kD8FromPool2c9fd90 = 0x000000000000000fULL;  // 0x2c9fd90
constexpr std::uint64_t kD9FromPool2c9fd98 = 0x3f3333333f800000ULL;  // 0x2c9fd98: {1.0f, 0.7f}
constexpr std::uint64_t kD11FromPool2c9fda0 = 0x42e000003e800000ULL;  // 0x2c9fda0: {0.25f, 112.0f}
constexpr std::uint64_t kD10FromPool2c30168 = 0x0000000000000001ULL;  // 0x2c30168
constexpr std::uint64_t kD0FromPool2c9fd80 = 0x00000004ffffffffULL;  // 0x2c9fd80
constexpr std::uint64_t kX8Ptrs = 0x0000000a0000000aULL;              // mov x8,#0xa; movk x8,#0xa,LSL#32
// -0xfffb00000001 with bits 48..63 cleared (mov x8,#-0xfffb00000001; movk x8,#0,LSL#48).
constexpr std::uint64_t kX8Neg = (~std::uint64_t{0xfffb00000001ULL} + 1) & 0x0000ffffffffffffULL;
// movi d8, #-0x100000000.
constexpr std::uint64_t kD8Neg = 0xffffffff00000000ULL;
// x28 = 0x3f800000 | (0x48 << 32); x29 = 0x3d4ccccd; x20 = 0x40000000 | (2 << 32).
constexpr std::uint64_t kX28 = 0x000000483f800000ULL;
constexpr std::uint64_t kX29 = 0x000000003d4ccccdULL;
constexpr std::uint64_t kX20 = 0x0000000240000000ULL;

}  // namespace

// Vtable address of CR15Game is the value at image slot 0x3816230 (loader-filled; its bytes are not in the file).
extern const std::byte* const kCR15GameVtableSlot;      // *(u64*)0x03816230
// Loader-filled pointer at image slot 0x0382adc8: the source of two 16-byte copies.
extern const std::byte* const kCopySource382adc8;       // *(u64*)0x0382adc8
// Loader-filled CInputDevice* at image slot 0x0386ce10.
extern const std::byte* const kInputDevice386ce10;      // *(u64*)0x0386ce10
// Loader-filled pointer at image slot 0x03824368 (value V); the 8-byte word *V is stored at 0x03832078+0x10.
extern const std::int64_t* const kGot3824368;           // *(u64*)0x03824368
// Two image words at 0x0386c560 (copied to the static block at 0x03832078).
extern const std::uint64_t kPair386c560[2];             // 0x0386c560
// Writable static block at 0x03832078 (0x18 bytes).
extern std::byte g_static3832078[0x18];

// The string literals at 0x02ca13aa ("slot0") and 0x02ca13b0 ("No Title"), copied into the two name buffers.
extern const char kImage2ca13aa[];
extern const char kImage2ca13b0[];

// Eight-byte words at image addresses that the classifier chain reads (aligned down to 8).
// None equals -1 in the file (read at 0x2ca2418..0x2ca2467 and 0x2ca11c0..0x2ca1267), so the chain stores nothing.
extern const std::int64_t kWord2ca2418;
extern const std::int64_t kWord2ca2428;
extern const std::int64_t kWord2ca2440;
extern const std::int64_t kWord2ca11c0;
extern const std::int64_t kWord2ca1260;
extern const std::int64_t kWord2ca2450;
extern const std::int64_t kWord2ca2468;

namespace {

// Store helpers: each writes a value at a raw byte offset of the object (memcpy, no alignment assumed).
template <typename T>
void Put(std::byte* base, std::size_t offset, T value) {
  std::memcpy(base + offset, &value, sizeof value);
}

void PutQuad(std::byte* base, std::size_t offset, const Quad& value) {
  std::memcpy(base + offset, &value, sizeof value);
}

// Copies n bytes from src (a 16-byte copy from a loader-filled pointer, or a literal).
void PutBytes(std::byte* base, std::size_t offset, const std::byte* src, std::size_t n) {
  std::memcpy(base + offset, src, n);
}

}  // namespace

// Callees: each is the real game function that its thunk reaches, with the thunk's address.

// NRadEngine::NRadGame::CRxGame::CRxGame(NRadEngine::CMainArgs const&) @0x25e7808, thunk 0x1015a90.
void CRxGameConstruct(std::byte* self, const std::byte* args);
// NRadEngine::CMemoryContext::CurrentPtr() @0x12cae90, thunk 0xffbc50: the current CAllocator*.
std::byte* CMemoryContextCurrentPtr();
// NRadEngine::CMemBlock::CMemBlock(unsigned long long, unsigned long long, NRadEngine::CAllocator*)
// @0x12dbcc8, thunk 0xfedec0.
void CMemBlockCtor(std::byte* block, std::uint64_t size, std::uint64_t align, std::byte* allocator);
// NRadEngine::CMemory::Fill(void*, unsigned char, unsigned long long) @0x12cafcc, thunk 0x1047300;
// its body is a tail jump to memset.
void CMemoryFill(std::byte* dst, unsigned char value, std::uint64_t count);
// NRadEngine::CGFullScreenEffectSettings::CGFullScreenEffectSettings() @0x28fddf0, thunk 0x1016600.
void CGFullScreenEffectSettingsCtor(std::byte* self);
// NRadEngine::CInputListener::CInputListener(NRadEngine::CInputDevice*) @0x26379f4, thunk 0x1034dd0.
void CInputListenerCtor(std::byte* self, const std::byte* device);
// NRadEngine::NRadGame::CR15GameplayConstraintSolver::CR15GameplayConstraintSolver() @0x13069f0, thunk 0x103a180.
void CR15GameplayConstraintSolverCtor(std::byte* self);
// NRadEngine::CJson::CJson() @0x10a2250, thunk 0xfd7760.
void CJsonCtor(std::byte* self);
// NRadEngine::CPhTarget::ValidateTag() const @0x1aa2664, thunk 0x1045470; its body is a bare ret.
void CPhTargetValidateTag(const std::byte* self);
// NRadEngine::CMemory::Move(void*, void const*, unsigned long long) @0x12cb90c, thunk 0x1010a90; its body
// is a tail jump to memmove (thunk 0xffa880).
void CMemoryMove(std::byte* dst, const std::byte* src, std::uint64_t count);
// strlen: the stub at 0xff4b80 (thunk to GOT slot 0x37d1ca8, which the import table does not list). Ghidra's
// label PEM_read_bio_SSL_SESSION is wrong: both call sites pass a string literal, clamp the result to
// 0x200 or 0x80 and use it as the memmove length.
std::uint64_t StrLenStub(const char* s);
// NRadEngine::NRadGame::GfxQualityTemplate(EGfxQualityLevel, SR15GfxQualitySettings&) @0x21e8ca4,
// thunk 0xfe4110.
void GfxQualityTemplate(std::uint32_t level, std::byte* settings);

namespace NRadEngine::NRadGame {

void CR15Game::Construct(const std::byte* args) {
  std::byte* const self = reinterpret_cast<std::byte*>(this);

  CRxGameConstruct(self, args);                                      // 0x12fce74
  Put(self, 0x7b00, std::uint64_t{0});
  Put(self, 0x7ad8, std::uint64_t{0x40000000});
  Put(self, 0x00, kCR15GameVtableSlot + 0x10);                       // vptr (vtable + 0x10)
  Put(self, 0x7b08, std::uint32_t{0});
  Put(self, 0x7b10, kAllOnes);
  Put(self, 0x7b18, kAllOnes);

  std::byte* allocator = CMemoryContextCurrentPtr();                 // 0x12fcea4
  CMemBlockCtor(self + 0x7b20, 0, 0, allocator);                     // 0x12fcebc
  Put(self, 0x7b50, std::uint64_t{0});
  Put(self, 0x7b40, std::uint64_t{0x20});
  Put(self, 0x7b48, std::uint64_t{0});
  allocator = CMemoryContextCurrentPtr();                            // 0x12fced0
  CMemBlockCtor(self + 0x7b58, 0, 0, allocator);                     // 0x12fcee8
  Put(self, 0x7b78, std::uint64_t{0x20});
  Put(self, 0x7b88, std::uint64_t{0});
  Put(self, 0x7b80, std::uint64_t{0});
  allocator = CMemoryContextCurrentPtr();                            // 0x12fcef8
  CMemBlockCtor(self + 0x7b90, 0, 0, allocator);                     // 0x12fcf10
  Put(self, 0x7bb0, std::uint64_t{0x20});
  Put(self, 0x7bc0, std::uint64_t{0});
  Put(self, 0x7bb8, std::uint64_t{0});
  allocator = CMemoryContextCurrentPtr();                            // 0x12fcf20
  CMemBlockCtor(self + 0x7bc8, 0, 0, allocator);                     // 0x12fcf38

  // Buffer at +0x7c08 (0x200 bytes) and buffer at +0x7e08 (0x80 bytes): zeroed, then the names below.
  std::byte* const name1 = self + 0x7c08;
  Put(self, 0x7be8, std::uint64_t{0x20});
  Put(self, 0x7bf8, std::uint64_t{0});
  Put(self, 0x7bf0, std::uint64_t{0});
  CMemoryFill(name1, 0, 0x200);                                      // 0x12fcf5c
  std::byte* const name2 = self + 0x7e08;                            // x20 from here on
  CMemoryFill(name2, 0, 0x80);                                       // 0x12fcf74
  Put(self, 0x7e9c, std::uint64_t{0});                               // str d0 (zero) at x27+0x394
  CGFullScreenEffectSettingsCtor(self + 0x7ea8);                     // 0x12fcf8c
  CInputListenerCtor(self + 0x8440, kInputDevice386ce10);            // 0x12fcfa0: x1 loaded from 0x386ce10

  // Classifier: the seven aligned words are compared with -1 in order; the first match stores its index.
  Put(self, 0x84d0, std::uint64_t{0});                               // 0x12fcfac
  Put(self, 0x84f0, kAllOnes);                                       // 0x12fcfb0
  Put(self, 0x8518, std::uint64_t{0});                               // 0x12fcfb4
  std::uint64_t classifier = 0;
  if (kWord2ca2418 == -1) {
    classifier = 1;
  } else if (kWord2ca2428 == -1) {
    classifier = 2;
  } else if (kWord2ca2440 == -1) {
    classifier = 3;
  } else if (kWord2ca11c0 == -1) {
    classifier = 4;
  } else if (kWord2ca1260 == -1) {
    classifier = 7;
  } else if (kWord2ca2450 == -1) {
    classifier = 0x10;
  } else if (kWord2ca2468 == -1) {
    classifier = 6;
  }
  if (classifier != 0) {
    Put(self, 0x8518, classifier);                                   // 0x12fd090
  }
  Put(self, 0x8520, std::uint64_t{0});                               // 0x12fd094

  // Two 16-byte copies from the loader-filled pointer at 0x382adc8, then the -1 and zero words.
  const std::byte* const copy_source = kCopySource382adc8;
  PutBytes(self, 0x8528, copy_source, 16);                           // 0x12fd0ac
  PutBytes(self, 0x8538, copy_source, 16);                           // 0x12fd0b8
  PutQuad(self, 0x8548, kMinusOneLow);                               // 0x12fd0cc: image pair {-1, 0}
  Put(self, 0x855e, std::uint16_t{0});                               // 0x12fd0dc: strh wzr, [x9,#4]
  Put(self, 0x855a, std::uint32_t{0});                               // 0x12fd0e0: str wzr, [x9]
  Put(self, 0x8560, kAllOnes);                                       // 0x12fd0e8: stp x23, x23
  Put(self, 0x8568, kAllOnes);
  Put(self, 0x8578, std::uint64_t{0});                               // 0x12fd0f0
  Put(self, 0x858e, kAllOnes);                                       // 0x12fd0f4
  Put(self, 0x8588, kAllOnes);                                       // 0x12fd0fc
  Put(self, 0x8580, kAllOnes);                                       // 0x12fd104
  Put(self, 0x8596, std::uint16_t{0});                               // 0x12fd108
  Put(self, 0x8598, std::uint64_t{0xffff});                          // 0x12fd10c: x22 = 0xffff here
  Put(self, 0x85c0, std::uint16_t{0xffff});                          // 0x12fd110
  Put(self, 0x85a8, kAllOnes);                                       // 0x12fd114
  Put(self, 0x85b0, kAllOnes);                                       // 0x12fd11c
  Put(self, 0x85a0, kAllOnes);                                       // 0x12fd124
  Put(self, 0x85c2, std::uint16_t{0});                               // 0x12fd130
  Put(self, 0x85c4, std::uint32_t{0});                               // 0x12fd134
  Put(self, 0x85cc, std::uint32_t{0});                               // 0x12fd138
  CR15GameplayConstraintSolverCtor(self + 0x85d0);                   // 0x12fd13c

  CJsonCtor(self + 0x89c0);                                          // 0x12fd148
  Put(self, 0x89d0, kAllOnes);                                       // 0x12fd158
  Put(self, 0x89e0, kAllOnes);                                       // 0x12fd15c
  Put(self, 0x89f8, std::uint64_t{0});                               // 0x12fd160
  PutQuad(self, 0x89e8, kZero16);                                    // 0x12fd164
  allocator = CMemoryContextCurrentPtr();                            // 0x12fd168
  CMemBlockCtor(self + 0x8a00, 0, 0, allocator);                     // 0x12fd180

  Put(self, 0x8a20, std::uint64_t{0x20});                            // 0x12fd198: x24 = 0x20
  Put(self, 0x8a38, std::uint64_t{0});                               // 0x12fd19c
  PutQuad(self, 0x8a28, kZero16);                                    // 0x12fd1b4
  Put(self, 0x8a40, kAllOnes);                                       // 0x12fd1b8
  Put(self, 0x8a58, kAllOnes);                                       // 0x12fd1bc
  Put(self, 0x8a60, kAllOnes);
  Put(self, 0x8a50, kAllOnes);                                       // 0x12fd1c0
  Put(self, 0x8a68, std::uint64_t{0});                               // 0x12fd1c4
  CMemoryFill(self + 0x8a70, 0, 0x80);                               // 0x12fd1c8
  CMemoryFill(self + 0x8af0, 0, 0x80);                               // 0x12fd1dc
  CMemoryFill(self + 0x8b70, 0, 0x80);                               // 0x12fd1f0
  CJsonCtor(self + 0x8bf0);                                          // 0x12fd1fc

  // Settings defaults, the GfxQuality inputs, and the Fill calls at 0x12fd204..0x12fd2d8.
  PutQuad(self, 0x8c04, kSettingsCurrent);                           // 0x12fd204: q1 at x27+0x10fc
  PutQuad(self, 0x8c14, kFourTens);                                  // 0x12fd228
  PutQuad(self, 0x8c24, kFloatsQ2);                                  // 0x12fd23c
  Put(self, 0x8c34, kD8FromPool2c76068);                             // 0x12fd248
  PutQuad(self, 0x8c3c, kZero16);                                    // 0x12fd250
  Put(self, 0x8c4c, kD0FromPool2c9fd88);                             // 0x12fd258
  PutQuad(self, 0x8c54, kZero16);                                    // 0x12fd270
  CMemoryFill(self + 0x8c64, 0, 0x80);                               // 0x12fd274
  PutQuad(self, 0x8ce4, kSettingsPending);                           // 0x12fd27c..0x12fd28c
  Put(self, 0x8cf4, kX8Ptrs);                                        // 0x12fd29c
  Put(self, 0x8cfc, std::uint32_t{0xa});                             // 0x12fd2a8
  PutQuad(self, 0x8d00, kFloatsQ2);                                  // 0x12fd2ac: q0 reloaded from the stack
  Put(self, 0x8d10, kD8FromPool2c76068);                             // 0x12fd2b8
  PutQuad(self, 0x8d18, kZero16);                                    // 0x12fd2bc
  Put(self, 0x8d28, kD0FromPool2c9fd88);                             // 0x12fd2d0
  PutQuad(self, 0x8d30, kZero16);                                    // 0x12fd2d4
  CMemoryFill(self + 0x8d40, 0, 0x80);                               // 0x12fd2d8

  // The GfxQualityTemplate(2, settings) block at 0x12fd2dc..0x12fd358.
  Put(self, 0x8dc0, std::uint64_t{0});                               // 0x12fd328
  Put(self, 0x8dd8, kX28);                                           // 0x12fd32c
  Put(self, 0x8de0, kX29);                                           // 0x12fd330
  Put(self, 0x8de8, kX20);                                           // 0x12fd334
  Put(self, 0x8dc8, kD8FromPool2c9fd90);                             // 0x12fd338
  Put(self, 0x8dd0, kD9FromPool2c9fd98);                             // 0x12fd33c
  Put(self, 0x8e10, std::uint64_t{0});                               // 0x12fd340
  PutQuad(self, 0x8df0, kZero16);                                    // 0x12fd344
  PutQuad(self, 0x8e00, kZero16);                                    // 0x12fd348
  Put(self, 0x8e18, kD10FromPool2c30168);                            // 0x12fd34c
  Put(self, 0x8e20, kD11FromPool2c9fda0);                            // 0x12fd350
  Put(self, 0x8e28, std::uint32_t{0});                               // 0x12fd354
  GfxQualityTemplate(2, self + 0x8df0);                              // 0x12fd358

  // The second GfxQualityTemplate(2, settings +0x8e54) block at 0x12fd35c..0x12fd3bc.
  Put(self, 0x8e2c, kD8FromPool2c9fd90);                             // 0x12fd360
  Put(self, 0x8e34, kD9FromPool2c9fd98);                             // 0x12fd368
  Put(self, 0x8e3c, kX28);                                           // 0x12fd370
  Put(self, 0x8e44, kX29);                                           // 0x12fd37c
  Put(self, 0x8e4c, kX20);                                           // 0x12fd38c
  PutQuad(self, 0x8e64, kZero16);                                    // 0x12fd398
  Put(self, 0x8e74, std::uint64_t{0});                               // 0x12fd3a8
  PutQuad(self, 0x8e54, kZero16);                                    // 0x12fd3ac
  Put(self, 0x8e7c, kD10FromPool2c30168);                            // 0x12fd3b0
  Put(self, 0x8e84, kD11FromPool2c9fda0);                            // 0x12fd3b4
  Put(self, 0x8e8c, std::uint32_t{0});                               // 0x12fd3b8
  GfxQualityTemplate(2, self + 0x8e54);                              // 0x12fd3bc

  // The CMemBlock at 0x8ea0 and the -1 / zero stores at 0x12fd3c0..0x12fd478.
  Put(self, 0x8e96, kAllOnes);                                       // 0x12fd3c8: stur x23, [x8,#6]
  Put(self, 0x8e90, kAllOnes);                                       // 0x12fd3cc
  Put(self, 0x8e9e, std::uint16_t{0});                               // 0x12fd3d0
  allocator = CMemoryContextCurrentPtr();                            // 0x12fd3d4
  CMemBlockCtor(self + 0x8ea0, 0, 0, allocator);                     // 0x12fd3ec
  Put(self, 0x8ec0, std::uint64_t{0x20});                            // 0x12fd3f8
  PutQuad(self, 0x8ec8, kZero16);                                    // 0x12fd400
  Put(self, 0x8edc, std::uint64_t{0});                               // 0x12fd408
  Put(self, 0x8ee4, std::uint32_t{0});                               // 0x12fd410
  Put(self, 0x8ee8, kAllOnes);                                       // 0x12fd418
  Put(self, 0x8ef0, kAllOnes);                                       // 0x12fd420
  Put(self, 0x8ef6, kAllOnes);                                       // 0x12fd428
  Put(self, 0x8efe, std::uint16_t{0});                               // 0x12fd430
  Put(self, 0x8f00, std::uint64_t{0xffff});                          // 0x12fd434
  Put(self, 0x8f28, std::uint16_t{0xffff});                          // 0x12fd438
  Put(self, 0x8f10, kAllOnes);                                       // 0x12fd43c
  Put(self, 0x8f18, kAllOnes);                                       // 0x12fd444
  Put(self, 0x8f08, kAllOnes);                                       // 0x12fd44c
  Put(self, 0x8f2a, std::uint16_t{0});                               // 0x12fd454
  Put(self, 0x8f2c, std::uint32_t{0});                               // 0x12fd458
  Put(self, 0x8f40, kAllOnes);                                       // 0x12fd45c
  Put(self, 0x8f30, kAllOnes);                                       // 0x12fd46c
  Put(self, 0x8f38, kAllOnes);                                       // 0x12fd470: str x23, [x0], x0 = self+0x8f38
  Put(self, 0x8f48, kD8Neg);                                         // 0x12fd474: stur d8, [x26,#0xaa]
  CPhTargetValidateTag(self + 0x8f38);                               // 0x12fd478

  Put(self, 0x8f50, kX8Neg);                                         // 0x12fd47c: stur x8, [x26,#0xb2]
  PutQuad(self, 0x8f78, kZero16);                                    // 0x12fd490
  PutQuad(self, 0x8f68, kZero16);                                    // 0x12fd4a4
  Put(self, 0x8f88, std::uint32_t{0});                               // 0x12fd4b0
  PutQuad(self, 0x8f58, kZero16);                                    // 0x12fd4bc
  Put(self, 0x8f9c, std::uint32_t{0x3f800000});                      // 0x12fd4c4: stur w24, [x26,#0xfe]
  PutQuad(self, 0x8f8c, kQ1);                                        // 0x12fd4c8: stur q1, [x26,#0xee]
  Put(self, 0x8fac, std::uint32_t{0});                               // 0x12fd4cc
  Put(self, 0x8fa4, std::uint64_t{0});                               // 0x12fd4d4
  Put(self, 0x8fb0, kAllOnes);                                       // 0x12fd4dc
  Put(self, 0x8fb8, kAllOnes);                                       // 0x12fd4e4
  Put(self, 0x8fbe, kAllOnes);                                       // 0x12fd4ec
  Put(self, 0x8fc6, std::uint16_t{0});                               // 0x12fd4f4: strh wzr, [x25,#0x26]
  Put(self, 0x8fc8, std::uint64_t{0xffff});                          // 0x12fd4f8: str x22, [x25,#0x28]
  Put(self, 0x8ff0, std::uint16_t{0xffff});                          // 0x12fd4fc: strh w22, [x25,#0x50]
  Put(self, 0x8fd8, kAllOnes);                                       // 0x12fd500
  Put(self, 0x8fe0, kAllOnes);                                       // 0x12fd508
  Put(self, 0x8fd0, kAllOnes);                                       // 0x12fd514
  Put(self, 0x8ff2, std::uint16_t{0});                               // 0x12fd518
  Put(self, 0x8ff4, std::uint32_t{0});                               // 0x12fd51c
  Put(self, 0x9008, kAllOnes);                                       // 0x12fd520
  Put(self, 0x8ff8, kAllOnes);                                       // 0x12fd52c
  Put(self, 0x9000, kAllOnes);                                       // 0x12fd530: str x23, [x0], x0 = self+0x9000
  Put(self, 0x9010, kD8Neg);                                         // 0x12fd534: str d8, [x25,#0x70]
  CPhTargetValidateTag(self + 0x9000);                               // 0x12fd538

  // 0x12fd53c..0x12fd5b0: the 16-byte image pairs, the loader-filled words, and the static block.
  Put(self, 0x9018, kD0FromPool2c9fd80);                             // 0x12fd550: str d0, [x25,#0x78]
  Put(self, 0x9050, std::uint32_t{0});                               // 0x12fd554
  const std::int64_t global_word = *kGot3824368;                     // 0x12fd558..0x12fd590: *(*(0x3824368))
  PutQuad(self, 0x9040, kZero16);                                    // 0x12fd574
  PutQuad(self, 0x9030, kZero16);                                    // 0x12fd578
  PutQuad(self, 0x9020, kZero16);                                    // 0x12fd57c
  PutQuad(self, 0x9054, kQ1);                                        // 0x12fd580: stur q1, [x25,#0xb4]
  Put(self, 0x9064, std::uint32_t{0x3f800000});                      // 0x12fd584: str w24, [x25,#0xc4]
  Put(self, 0x8558, std::uint16_t{0xffff});                          // 0x12fd588: strh w22, [x27,#0xa50]
  std::byte* const static_block = g_static3832078;                   // 0x3832078
  Put(static_block, 0x00, kPair386c560[0]);                          // 0x12fd58c..0x12fd5a4: ldp x10, x8
  Put(static_block, 0x08, kPair386c560[1]);
  Put(static_block, 0x10, global_word);                              // 0x12fd5a8
  Put(self, 0x7af0, std::uint64_t{0});                               // 0x12fd5ac
  PutQuad(self, 0x7ae0, kZero16);                                    // 0x12fd5b0

  // First name: copied unless buffer 1 already is the literal at 0x2ca13aa (b.eq 0x12fd628).
  if (reinterpret_cast<const char*>(name1) != kImage2ca13aa) {
    const char* const literal1 = kImage2ca13aa;                      // 0x12fd5b8
    const std::uint64_t length1 = StrLenStub(literal1);              // 0x12fd5c4
    const std::uint64_t count1 = length1 < 0x200 ? length1 : 0x200;  // csel, unsigned lower
    CMemoryMove(name1, reinterpret_cast<const std::byte*>(literal1), count1);  // 0x12fd5e4
    if (length1 <= 0x1ff) {                                          // b.hi 0x12fd5fc
      Put(self, 0x7c08 + count1, std::uint8_t{0});                   // 0x12fd5f0..0x12fd5f8
    }
    Put(self, 0x7e07, std::uint8_t{0});                              // 0x12fd604: strb at +0x7c08+0x1ff
    if (length1 + 1 <= 0x1ff) {                                      // b.hi 0x12fd628
      std::memset(self + 0x7c08 + length1 + 1, 0, 0x1ff - length1);  // 0x12fd624
    }
  }

  // Second name: copied unless buffer 2 already is the literal at 0x2ca13b0 (b.eq 0x12fd6a8).
  if (reinterpret_cast<const char*>(name2) != kImage2ca13b0) {
    const char* const literal2 = kImage2ca13b0;                      // 0x12fd638
    const std::uint64_t length2 = StrLenStub(literal2);              // 0x12fd644
    const std::uint64_t count2 = length2 < 0x80 ? length2 : 0x80;    // csel, unsigned lower
    CMemoryMove(name2, reinterpret_cast<const std::byte*>(literal2), count2);  // 0x12fd664
    if (length2 <= 0x7f) {                                           // b.hi 0x12fd67c
      Put(self, 0x7e08 + count2, std::uint8_t{0});                   // 0x12fd68c..0x12fd690
    }
    Put(self, 0x7e87, std::uint8_t{0});                              // 0x12fd684: strb at +0x7e08+0x7f
    if (length2 + 1 <= 0x7f) {                                       // b.hi 0x12fd6a8
      std::memset(self + 0x7e08 + length2 + 1, 0, 0x7f - length2);   // 0x12fd6a4
    }
  }

  // Common tail at 0x12fd6a8..0x12fd6f8.
  PutQuad(self, 0x7e88, kQ0);                                        // 0x12fd6ac..0x12fd6bc
  Put(self, 0x7e98, std::uint32_t{0});                               // 0x12fd6c8
  Put(self, 0x7ea4, std::uint32_t{0});                               // 0x12fd6cc
  Put(self, 0x7c00, std::uint64_t{0});                               // 0x12fd6d0
  Put(self, 0x8570, std::uint64_t{0});                               // 0x12fd6d4
  Put(self, 0x85c8, std::uint32_t{0});                               // 0x12fd6d8
  Put(self, 0x7af8, std::uint64_t{0});                               // 0x12fd6dc
  Put(self, 0x7b0c, std::uint32_t{0});                               // 0x12fd6e0
  Put(self, 0x8ed8, std::uint32_t{0});                               // 0x12fd6e4
  Put(self, 0x8fa0, std::uint32_t{0});                               // 0x12fd6e8
  PutQuad(self, 0x8430, kZero16);                                    // 0x12fd6ec
  Put(self, 0x84d8, std::uint64_t{0});                               // 0x12fd6f0
  PutQuad(self, 0x84e0, kZero16);                                    // 0x12fd6f4
  PutQuad(self, 0x84f8, kZero16);                                    // 0x12fd6f8: stp q0, q0
  PutQuad(self, 0x8508, kZero16);
}

}  // namespace NRadEngine::NRadGame
