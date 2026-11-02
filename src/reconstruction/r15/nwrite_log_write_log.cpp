// NRadEngine::NWriteLog::WriteLog
// Quest libr15.so VA 0x1088c20; ReVault binary 576.
// ReVault raw parent 100208004, reconstruction node 100600529; grade H: the node's @confidence is H, and its comments tie the 0x1088c20..0x1088c80 listing to the AAPCS64 va_start prologue and the callee to thunk_FUN_01088720.
// @0x1088c20 — NRadEngine::NWriteLog::WriteLog
/* @confidence: H */
#include <cstdarg>

namespace NRadEngine {
namespace NWriteLog {

enum ELogLevel : int;

// /* thunk_FUN_01088720 @ 0xfe7590 */ formats and emits the record. x3 points at
// the va_list built below, so the fourth parameter is a pointer, not a copy.
void WriteLogV(ELogLevel level, unsigned long long channel, const char* format,
               va_list* args);

// The listing (0x1088c20..0x1088c80) is exactly the AAPCS64 va_start prologue:
//   [sp+0x00..0x7f]  q0..q7 save area (FP variadic arguments)
//   [sp+0x80..0xaf]  __vr_top = sp+0x80
//   [sp+0x88..0xa8]  x3..x7 saved (integer variadic arguments)
//   [sp+0xb0..0xcf]  the va_list: {__stack = sp+0x100, __gr_top = sp+0xb0,
//                    __vr_top = sp+0x80, __gr_offs|__vr_offs = 0xffffff80ffffffd8}
//   [sp+0xd0..0xf0]  scratch, then the saved lr restored at 0x1088c78
// The packed constant is gr_offs = -40 (x3..x7) in the low word and vr_offs =
// -128 (q0..q7) in the high word, stored at [sp+0xe8] (0x1088c40).
void WriteLog(ELogLevel level, unsigned long long channel, const char* format, ...) {
  va_list args;
  va_start(args, format);
  WriteLogV(level, channel, format, &args);
  va_end(args);
}

}  // namespace NWriteLog
}  // namespace NRadEngine
