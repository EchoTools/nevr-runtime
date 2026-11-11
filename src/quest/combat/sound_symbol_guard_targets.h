/* The pinned slots, functions, code words and data facts the sound-symbol guard relies on (the guard drops a post whose CSoundSymbol is 0 or the
 * invalid id; the Quest side is tracked in #1737).
 *
 * Every address is an ELF virtual address in the store libr15 (the store file's build ID is `kLibR15StockBuildId` in pinned_targets.h); the disassembly's address is
 * the ELF address plus 0x100000. src/quest/tests/sound_symbol_guard_pinned_test.cpp checks each slot, word and fact against the real file
 * (`just test-quest-hooks-pinned`, which needs the store APK). The store libr15 (sha256 8dd9a961...) relocations, from `llvm-readelf -rW`:
 *
 *   CSoundCS::PlaySound        JUMP_SLOT 0x36dd210 and GLOB_DAT 0x37148e0, both -> 0x1706904 (the GLOB_DAT is a function-pointer word)
 *   CSoundPlayer::Play3DSound  JUMP_SLOT 0x3701090 -> 0x134b468
 *
 * The bullet site, `bl` at 0x21230d0 inside CR15NetBulletCS::Update, calls Play3DSound through the PLT stub at 0xf53350 and returns to 0x21230d4.
 * The .bss word 0x37af76c is the default of the CSoundSymbol it passes: CR15NetBulletCS::Update loads it into w28 at 0x2122fdc and `mov w1, w28`
 * (0x21230c4) makes it the symbol argument. One path replaces w28 first: when the table lookup at 0x2123014..0x2123084 finds an index other than -1
 * (`cmn x11, #1` / `b.eq` at 0x2123084 not taken), `ldr w28, [x8, #0x8]` at 0x21230a0 loads that table entry's word. So the word is the
 * default, not the only source. `_INIT_355` (ReVault's name for 0xf70e14, listed in .init_array) writes the word once, at load, with the
 * .rodata word 0x31c8330 (0x1406b30a), so the default is 0x1406b30a and not zero. ReVault (libr15.so) lists no other write to 0x37af76c.
 *
 * The crash offset 0x11d1cac from upstream's notes is the return address after the `bl` at 0x11d1ca8, which calls the C1 constructor of
 * CConsoleStream through a PLT stub, inside OutputError (0x11d1b38, 1268 bytes). It is not a faulting instruction: a crash that logs +0x11d1cac is
 * a return from that call, so the fault is inside a callee.
 *
 * What a post dropped before Play3DSound leaves undone. Play3DSound (this, w1 symbol, w2 emitter id, s0 volume) has these exits:
 * audio disabled -> `mov w0, wzr` at 0x134b488, return 0, nothing touched; emitter id not found -> `mov w0, #-1` at 0x134b62c (reached from
 * 0x134b530 and 0x134b558 without reading the emitter); no free sound (0x134b588, the "max sounds" line), a null sound (0x134b5ac) or a post
 * that OutputError reports (`cbnz w0` at 0x134b5fc, after DestroySoundNoLock at 0x134b658) -> `ldrb w8, [x20, #0xbc]` and a bit-0 test
 * (0x134b624/0x134b628, 0x134b660/0x134b664): bit set -> DestroyEmitterNoLock (`bl` 0x134b6c0) and -1; bit clear -> -1, emitter kept.
 * Bit 0 of emitter+0xbc is "destroy this emitter when its sound does not start": CreateEmitterNoLock sets it to (its first unsigned != 0)
 * (`mov w19, w2` 0x1347e6c; `cmp w19, #0` / `orr #1` / `and #~1` / `csel` / `str [x20, #0xbc]` at 0x1348010-0x1348028, the same at
 * 0x1348088-0x134809c and 0x13480b8-0x13480c8), and CreateEmitter passes its w2 through (`mov w3, wzr` 0x1348adc, `bl` 0x1348ae0).
 * RemoveSound reads it too (0x1349fb0-0x1349fc4: bit set and no sounds left -> DestroyEmitterNoLock). The other writers clear it or set
 * other bits: the constructor (0x1343c70), DestroyEmitterNoLock (0x1348208) and DestroyAllEmitters (0x1346c60, 0x1346cb4) store 0,
 * CSoundPlayer::Initialize clears it on its fixed emitters (0x1346020, 0x13462c0) and ORs 0x50 (0x13461ec), PlayOneOff ORs 0x10 (0x134b26c). CSoundEmitter::Initialize (0x1343d74) also sets it from its first argument,
 * and nothing in libr15 calls it (no `bl`, no relocation). The emitters of the two sites the guard is for never carry the bit:
 *   - PlaySound posts on entry+4 (`ldr w8, [x21, #0x4]` 0x170697c, `mov w2, w8` 0x1706984); CSoundCS::InitSoundCI stores that word from
 *     CreateEmitter(name, 0, 0) (`mov w2, wzr` 0x170452c, `bl` 0x1704534, `str w0, [x19, #0x4]` 0x1704538).
 *   - The bullet site posts on cs+0x1b0+4*i (`ldr w27, [x8, #0x1b0]` 0x2122fd0, `mov w2, w27` 0x21230cc); CR15NetBulletCS::Initialize
 *     fills that table from CreateEmitter(name, 0, 0) (`mov w2, wzr` 0x2121748, `bl` 0x2121750, `str w0, [x8, #0x1b0]` 0x2121764).
 * One caller of Play3DSound does post on a bit-0 emitter: CRxMaterialFXCS::TriggerSingle creates it with CreateEmitter(name, 1, 0)
 * (`mov w2, #0x1` 0x24bd514, `bl` 0x24bd520) and posts on it at 0x24bd5ac (return 0x24bd5b0), result unread. A drop there skips the
 * destroy a failed post would make; when every post of a TriggerSingle call is dropped, no sound starts on the emitter, so none ends to
 * let RemoveSound reap it, and it stays until DestroyAllEmitters (whose one caller is CSoundPlayer::Shutdown): a guard that drops on every
 * caller can leak one emitter per such TriggerSingle call (whether TriggerSingle frees it itself is not established). The
 * other emitter creator with a 1 is PlayOneOff (`mov w2, #0x1` 0x134b24c into CreateEmitterNoLock), which does not call Play3DSound.
 * Play3DSound's PLT stub (0xf53350) has 9 callers in the file: Play2DSound 0x134c2b8 (tail), CSoundVolume::PlayAmbientSounds 0x13516fc,
 * CAmbientSoundCS::PlayAmbientSound 0x1368b30, CAmbientSoundCS::SendEvent 0x136a704 (tail), PlaySound 0x1706988, PlaySoundAtAttach
 * 0x1706fd4, the bullet site 0x21230d0, TriggerSingle 0x24bd5ac, CSendAmbientSoundActionNode::Enter 0x272575c (`llvm-objdump -d` of the
 * whole file; no relocation other than the JUMP_SLOT names Play3DSound). Which emitter the five not named above post on is not established.
 * Read from the instruction words; the whole-function reconstructions of CreateEmitterNoLock, InitSoundCI, CR15NetBulletCS::Initialize,
 * TriggerSingle and RemoveSound are not reconstructed (reconstruction pending), so "no other writer of entry+4 / of the bullet table" is not established.
 *
 * Where the "not a valid wwise identifier" text is made, and where it is written. CWwiseEngine::PostSoundEvent (0x2549264, through
 * the PLT stub 0xf00000 that Play3DSound calls at 0x134b5e8) posts the event; when the post returns 0 (`cbz w0` 0x25492d0, and again after
 * the wait at 0x25493c8) it sets the playing id to -1 (0x25493cc/0x25493d8) and, when the symbol has no debug name (`cbz x0` 0x25493fc,
 * as in a final build), calls CErrMsg::CreateAndAddError with line 0x51 (0x254947c), the format at .rodata 0x31ec568 ("Error %s. It is
 * not a valid wwise identifier or registered sound symbol. ...", `add x3, x3, #0x568` 0x2549474) and the error type CSymbol64
 * 0x0f986c58c7b3d4fb, the .rodata quad at 0x31ec4f0, loaded by `add x8, x8, #0x4f4` (0x25493f0; it makes 0x31ec4f4), `and x8, x8, #~7` (0x25493f4; it aligns that down to 0x31ec4f0) and `ldr x2, [x8]` (0x25493f8), and returns its status.
 * So 0x0f986c58c7b3d4fb is the error record's type key, not a sound symbol: no CSoundSymbol of that value (or of its low word
 * 0xc7b3d4fb) is posted on this path. The text says only that PostEvent returned 0, for any symbol whose bank is not loaded or that no
 * bank defines. Play3DSound passes the status to OutputError(status, 8) (`bl` 0xf4e4c0 at 0x134b5f0); OutputError returns 0 at once for
 * status 0 or -1 (`add w8, w0, #1` / `cmp w8, #2` / `b.hs` 0x11d1b54-0x11d1b60, which is CreateAndAddError's answer with no error db), else
 * logs the callstack, constructs the CConsoleStream at 0x11d1ca8 (the return 0x11d1cac is upstream's crash frame) and serializes the
 * record at env+0x40008+(status-1)*0x18. When that record is incomplete it takes a null record (`mov x20, xzr` 0x11d1d5c) and still calls
 * through it (`ldr x8, [x20]` at 0x11d1ddc on the JSON path and 0x11d1ee4 on the text path): a null dereference inside OutputError. So
 * OutputError has a fault path on a non-zero status; whether CErrorDb::AddError ever returns a status whose record is incomplete, and
 * whether the CConsoleStream constructor can fault, are not established (both not reconstructed, reconstruction pending). Decompilations read:
 * the reconstructions of Play3DSound (0x144b468), OutputError (0x12d1b38), PostSoundEvent (0x2649264) and CreateAndAddError (0x12ce4dc)
 * in the project's reconstruction tree (disassembly VAs), each checked here against the store file's words above.
 *
 * Whether the three symbols are events of the store build's banks. The symbol reaches Wwise unchanged: Play3DSound passes its w1
 * masked to 32 bits (`and x1, x21, #0xffffffff` 0x134b5d8), PostSoundEvent hands it to PostWwiseEvent (0x294b500) as the event id
 * (`mov w0, w20` 0x25492a8), which passes it on as it is (the reconstruction of 0x2a4b500); that Wwise then matches it against the
 * HIRC event ids of the loaded banks is the Wwise SDK's AkUniqueID contract, an inference. tools/cmd/wwise-events read the two stock
 * package files of the store OBB (_data/5932408047/rad15/android/packages/48037dc70b0ecab2_0 and 2b47aab238f60515_0, each equal to its
 * digest in tools/quest_stock_data.pin), decompressed whole: 34 banks (27 and 7: every "BKHD" in the two streams), all version 134, no
 * malformed HIRC, 1295 distinct event ids. 0x1406b30a, 0x00000001 and 0xc7b3d4fb are none of them, and no HIRC object of any type has
 * those ids; the four bytes of 0x1406b30a occur nowhere in either decompressed package. So the bullet site's default symbol is not an event
 * of any stock bank: every post that keeps it fails in PostEvent and produces the "not a valid wwise identifier" record (above), the
 * per-shot print upstream measured. Not shown: that the game loads no bank from anywhere else (the OBB's other files are the two
 * manifests, and the APK holds no .bnk), and the run-time rate at which the default reaches the site.
 *
 * Upstream: `kotorvr/echoquest-combat` at `c5a67e8`, runtime/native/afkxr/afk_fx.cpp (the sound guards, around lines 5148-5264).
 *
 * PC (echovr.exe): echovr.exe CSoundCS::PlaySound is VA 0x1403f94c0, its play call is 0x14020b780, OutputError (0x14151ee70) is called at
 * 0x14020b8c1 after PostSoundEvent with no test of the symbol for 0, and whether that call faults on PC is not established.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/combat/pinned_words.h"

namespace nevr_quest::combat::sound_symbol_guard::targets {

inline constexpr const char* kPlaySymbol =
    "_ZN10NRadEngine8CSoundCS9PlaySoundENS_18SCompactPoolHandleENS_12CSoundSymbolEfNS_15SSoundDebugInfoE";
inline constexpr const char* kPlay3DSymbol = "_ZN10NRadEngine12CSoundPlayer11Play3DSoundENS_12CSoundSymbolEjf";

// The slots the guard would hook or check, and the function each holds in the pinned build.
struct Pins {
  std::uint64_t playSlot;      // PlaySound's JUMP_SLOT
  std::uint64_t playDataSlot;  // PlaySound's GLOB_DAT (a function-pointer word for the same function)
  std::uint64_t playFn;        // CSoundCS::PlaySound
  std::uint64_t play3DSlot;    // Play3DSound's JUMP_SLOT
  std::uint64_t play3DFn;      // CSoundPlayer::Play3DSound
};
inline constexpr Pins kPins = {0x36dd210ULL, 0x37148e0ULL, 0x1706904ULL, 0x3701090ULL, 0x134b468ULL};

// The code words the guard relies on: the bullet site's path to Play3DSound, the default symbol's reads, and the writer of the default
// (its base, its source and the store). The last entry is the .rodata word the writer copies, a data word the same check reads.
inline constexpr pinned::Word kWords[] = {
    {"Play3DSound stub: adrp x16, 0x3701000", 0xf53350, 0xd0013d70},
    {"Play3DSound stub: ldr x17, [x16, #0x90]", 0xf53354, 0xf9404a11},
    {"Play3DSound stub: add x16, x16, #0x90", 0xf53358, 0x91024210},
    {"Play3DSound stub: br x17", 0xf5335c, 0xd61f0220},
    {"bullet site: bl Play3DSound@plt", 0x21230d0, 0x97b8c0a0},
    {"bullet site: the return address (ldr x8, [x19, #0x278])", 0x21230d4, 0xf9413e68},
    {"default symbol read: adrp x8, 0x37af000", 0x2122fd4, 0xb000b468},
    {"default symbol read: ldr w28, [x8, #0x76c]", 0x2122fdc, 0xb9476d1c},
    {"default symbol writer (_INIT_355): adrp x29, 0x37af000", 0xf70e3c, 0xf00141fd},
    {"default symbol writer (_INIT_355): add x29, x29, #0x75c", 0xf70e4c, 0x911d73bd},
    {"default symbol source (_INIT_355): adrp x10, 0x31c8000", 0xf712d0, 0xf00112aa},
    {"default symbol source (_INIT_355): add x10, x10, #0x337", 0xf712d4, 0x910cdd4a},
    {"default symbol source (_INIT_355): and x10, x10, #-8", 0xf712d8, 0x927df14a},
    {"default symbol source (_INIT_355): ldr w9, [x10]", 0xf712e0, 0xb9400149},
    {"default symbol writer (_INIT_355): str w9, [x29, #0x10] (writes 0x37af76c)", 0xf712ec, 0xb90013a9},
    {"default symbol source word: .rodata 0x1406b30a", 0x31c8330, 0x1406b30a},
};
inline constexpr std::size_t kWordCount = sizeof(kWords) / sizeof(kWords[0]);
inline constexpr pinned::Pins kWordPins = {kWords, kWordCount, nullptr, 0};

// What a dropped post leaves undone, and where the invalid-id text is made and written (the paragraphs "What a post dropped before Play3DSound leaves undone" and "Where the not-a-valid-wwise-identifier text is made" above): Play3DSound's
// exits and its emitter-bit tests, the bit's writer, the emitters of PlaySound, the bullet site and TriggerSingle, and PlaySound's handling
// of the result.
inline constexpr pinned::Word kDropWords[] = {
    {"Play3DSound audio-disabled early-out: mov w0, wzr", 0x134b488, 0x2a1f03e0},
    {"Play3DSound: bl PostSoundEvent@plt", 0x134b5e8, 0x97eed286},
    {"Play3DSound: mov w1, #0x8 (OutputError's level)", 0x134b5ec, 0x52800101},
    {"Play3DSound: bl OutputError@plt", 0x134b5f0, 0x97f00bb4},
    {"Play3DSound: cbnz w0 (OutputError reported the post)", 0x134b5fc, 0x350001e0},
    {"Play3DSound: ldrb w8, [x20, #0xbc] (no sound)", 0x134b624, 0x3942f288},
    {"Play3DSound: tbnz w8, #0 -> destroy the emitter", 0x134b628, 0x37000208},
    {"Play3DSound: mov w0, #-1 (emitter not found, or bit 0 clear)", 0x134b62c, 0x12800000},
    {"Play3DSound: bl DestroySoundNoLock@plt (failed post)", 0x134b658, 0x97efeebe},
    {"Play3DSound: ldrb w8, [x20, #0xbc] (failed post)", 0x134b660, 0x3942f288},
    {"Play3DSound: tbz w8, #0 -> keep the emitter", 0x134b664, 0x3607fe48},
    {"Play3DSound: bl DestroyEmitterNoLock@plt", 0x134b6c0, 0x97ef3dec},
    {"CreateEmitterNoLock: mov w19, w2 (the destroy-on-fail flag)", 0x1347e6c, 0x2a0203f3},
    {"CreateEmitterNoLock: cmp w19, #0", 0x1348010, 0x7100027f},
    {"CreateEmitterNoLock: orr w9, w8, #0x1", 0x1348018, 0x32000109},
    {"CreateEmitterNoLock: and w8, w8, #0xfffffffe", 0x134801c, 0x121f7908},
    {"CreateEmitterNoLock: csel w8, w8, w9, eq", 0x1348024, 0x1a890108},
    {"CreateEmitterNoLock: str w8, [x20, #0xbc]", 0x1348028, 0xb900be88},
    {"CreateEmitter: mov w3, wzr (w2 passed through)", 0x1348adc, 0x2a1f03e3},
    {"CreateEmitter: bl CreateEmitterNoLock@plt", 0x1348ae0, 0x97ef9d00},
    {"RemoveSound: ldrb w8, [x1, #0xbc]", 0x1349fb0, 0x3942f028},
    {"RemoveSound: tbz w8, #0", 0x1349fb4, 0x360000a8},
    {"RemoveSound: bl DestroyEmitterNoLock@plt", 0x1349fc4, 0x97ef43ab},
    {"InitSoundCI: mov w2, wzr (PlaySound's emitter keeps bit 0 clear)", 0x170452c, 0x2a1f03e2},
    {"InitSoundCI: bl CreateEmitter@plt", 0x1704534, 0x97e00043},
    {"InitSoundCI: str w0, [x19, #0x4]", 0x1704538, 0xb9000660},
    {"PlaySound: ldr w8, [x21, #0x4] (the emitter)", 0x170697c, 0xb94006a8},
    {"PlaySound: and x1, x2, #0xffffffff (the 32-bit symbol)", 0x1706980, 0x92407c41},
    {"PlaySound: mov w2, w8", 0x1706984, 0x2a0803e2},
    {"PlaySound: bl Play3DSound@plt", 0x1706988, 0x97e13272},
    {"PlaySound: the return address (ldr w21, [x21, #0x4])", 0x170698c, 0xb94006b5},
    {"PlaySound: cmn w0, #1", 0x1706990, 0x3100041f},
    {"PlaySound: b.ne (any result but -1 is a start)", 0x1706994, 0x54000381},
    {"CR15NetBulletCS::Initialize: mov w2, wzr (bullet emitters keep bit 0 clear)", 0x2121748, 0x2a1f03e2},
    {"CR15NetBulletCS::Initialize: bl CreateEmitter@plt", 0x2121750, 0x97b78bbc},
    {"CR15NetBulletCS::Initialize: str w0, [x8, #0x1b0]", 0x2121764, 0xb901b100},
    {"bullet site: ldr w27, [x8, #0x1b0] (the emitter)", 0x2122fd0, 0xb941b11b},
    {"bullet site: mov w2, w27", 0x21230cc, 0x2a1b03e2},
    {"TriggerSingle: mov w2, #0x1 (its emitter carries bit 0)", 0x24bd514, 0x52800022},
    {"TriggerSingle: bl CreateEmitter@plt", 0x24bd520, 0x97a91c48},
    {"TriggerSingle: mov w2, w27", 0x24bd5a8, 0x2a1b03e2},
    {"TriggerSingle: bl Play3DSound@plt", 0x24bd5ac, 0x97aa5769},
    {"TriggerSingle: the return address (ldr w8, [x21, #0x18])", 0x24bd5b0, 0xb9401aa8},
    {"Play2DSound: b Play3DSound@plt", 0x134c2b8, 0x17f01c26},
    {"CSoundVolume::PlayAmbientSounds: bl Play3DSound@plt", 0x13516fc, 0x97f00715},
    {"CAmbientSoundCS::PlayAmbientSound: bl Play3DSound@plt", 0x1368b30, 0x97efaa08},
    {"CAmbientSoundCS::SendEvent: b Play3DSound@plt", 0x136a704, 0x17efa313},
    {"PlaySoundAtAttach: bl Play3DSound@plt", 0x1706fd4, 0x97e130df},
    {"CSendAmbientSoundActionNode::Enter: bl Play3DSound@plt", 0x272575c, 0x97a0b6fd},
    {"PostSoundEvent: cbz w0 (PostEvent returned 0)", 0x25492d0, 0x340003c0},
    {"PostSoundEvent: mov w8, #-1 (the playing id)", 0x25493cc, 0x12800008},
    {"PostSoundEvent: add x8, x8, #0x4f4 (the error type key)", 0x25493f0, 0x9113d108},
    {"PostSoundEvent: ldr x2, [x8]", 0x25493f8, 0xf9400102},
    {"PostSoundEvent: cbz x0 (no debug name for the symbol)", 0x25493fc, 0xb4000340},
    {"PostSoundEvent: add x3, x3, #0x568 (the invalid-id format)", 0x2549474, 0x9115a063},
    {"PostSoundEvent: mov w1, #0x51 (the line)", 0x254947c, 0x52800a21},
    {"PostSoundEvent: bl CErrMsg::CreateAndAddError@plt", 0x2549480, 0x97a62cfc},
    {"OutputError: add w8, w0, #0x1", 0x11d1b54, 0x11000408},
    {"OutputError: cmp w8, #0x2 (status 0 or -1 returns 0)", 0x11d1b58, 0x7100091f},
    {"OutputError: bl CConsoleStream::CConsoleStream@plt (returns to 0x11d1cac)", 0x11d1ca8, 0x97f53472},
    {"OutputError: mov x20, xzr (incomplete record)", 0x11d1d5c, 0xaa1f03f4},
    {"OutputError: ldr x8, [x20] (JSON path, through the record)", 0x11d1ddc, 0xf9400288},
    {"OutputError: ldr x8, [x20] (text path, through the record)", 0x11d1ee4, 0xf9400288},
};
inline constexpr pinned::Quad kDropQuads[] = {
    {"PostSoundEvent's error type CSymbol64 (.rodata)", 0x31ec4f0, 0x0f986c58c7b3d4fbULL},
    {"the invalid-id format's first 8 bytes, \"Error %s\" (.rodata)", 0x31ec568, 0x732520726f727245ULL},
};
inline constexpr pinned::Pins kDropPins = {kDropWords, sizeof(kDropWords) / sizeof(kDropWords[0]), kDropQuads,
                                           sizeof(kDropQuads) / sizeof(kDropQuads[0])};

// The functions the drop paragraphs name, by symbol, with the JUMP_SLOT each is called through (the PLT stubs the words call).
inline constexpr const char* kCreateEmitterSymbol = "_ZN10NRadEngine12CSoundPlayer13CreateEmitterEPKcjj";
inline constexpr const char* kCreateEmitterNoLockSymbol = "_ZN10NRadEngine12CSoundPlayer19CreateEmitterNoLockEPKcjj";
inline constexpr const char* kDestroyEmitterNoLockSymbol = "_ZN10NRadEngine12CSoundPlayer20DestroyEmitterNoLockEPNS_13CSoundEmitterE";
inline constexpr const char* kOutputErrorSymbol = "_ZN10NRadEngine11OutputErrorEiNS_9NWriteLog9ELogLevelE";
inline constexpr const char* kPostSoundEventSymbol =
    "_ZN10NRadEngine12CWwiseEngine14PostSoundEventENS_12CSoundSymbolEjNS_7CFlagsTIjLy1EEEPFviPvES4_jRj";
struct Called {
  const char* symbol;
  std::uint64_t slot;
  std::uint64_t fn;
};
inline constexpr Called kCalled[] = {
    {kCreateEmitterSymbol, 0x36d9a08ULL, 0x1348a74ULL},
    {kCreateEmitterNoLockSymbol, 0x36ef658ULL, 0x1347e4cULL},
    {kDestroyEmitterNoLockSymbol, 0x36e4e20ULL, 0x13480ecULL},
    {kOutputErrorSymbol, 0x36fe948ULL, 0x11d1b38ULL},
    {kPostSoundEventSymbol, 0x36d76e8ULL, 0x2549264ULL},
};

// The default CSoundSymbol: a .bss word (the test checks it lies in .bss), and the function listed in .init_array that writes it at load.
inline constexpr std::uint64_t kDefaultSymbolVaddr = 0x37af76c;
inline constexpr std::uint64_t kDefaultWriterFn = 0xf70e14;

}  // namespace nevr_quest::combat::sound_symbol_guard::targets
