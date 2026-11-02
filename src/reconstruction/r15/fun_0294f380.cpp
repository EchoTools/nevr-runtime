// FUN_0294f380 (audio command enqueue; unnamed in symbols)
// Quest libr15.so VA 0x2a4f380; ReVault binary 576.
// ReVault raw parent 100335554, reconstruction node 100600450; grade M: the 27-instruction body is exact; open only in the contracts of the four unnamed queue helpers kept as boundaries.
// @0x2a4f380 — FUN_0294f380 (audio command enqueue; unnamed in symbols)
/* @confidence: M — the 27-instruction body is exact (early-out on a zero id, a 16-bit
 * selector, three 32-bit fields at +0x04/+0x08/+0x0c of a 0x20-byte record, then a commit);
 * open only in the contracts of the four unnamed queue helpers kept as boundaries. */
#include <cstddef>
#include <cstdint>

namespace NRadEngine {
namespace {
// Record staged on the caller's stack and handed to the queue helpers. The helpers
// initialise it; this function only fills the three 32-bit fields.
constexpr std::size_t kRecordBytes = 0x20;  // 0x2a4f3a8: w1 = 0x20
constexpr std::size_t kField0 = 0x04;
constexpr std::size_t kField1 = 0x08;
constexpr std::size_t kField2 = 0x0c;
}  // namespace

// --- named external boundaries: unnamed engine command queue ---
// /* UNRESOLVED: FUN_02a52c80 */ returns the selector whose low 16 bits tag the record
// (0x2a4f3a0 masks with 0xffff).
std::uint32_t FUN_02a52c80();
// /* UNRESOLVED: FUN_02a490d0 */ prepares a record in caller-provided storage.
void FUN_02a490d0(void* record, std::uint32_t bytes, std::uint32_t selector);
// /* UNRESOLVED: FUN_02a490f0 */ returns the record being staged; called once per field.
std::uint32_t* FUN_02a490f0(void* record);
// /* UNRESOLVED: FUN_02a49100 */ commits the staged record.
void FUN_02a49100(void* record);

namespace {
void StoreField(void* record, std::size_t offset, std::uint32_t value) {
  *reinterpret_cast<std::uint32_t*>(
      reinterpret_cast<std::byte*>(FUN_02a490f0(record)) + offset) = value;
}
}  // namespace

void EnqueueAudioCommand(std::uint32_t id, std::uint32_t arg1, std::uint32_t arg2) {
  if (id == 0) {
    return;  // 0x2a4f380: cbz w0
  }

  alignas(8) std::byte record[kRecordBytes];
  const auto selector = FUN_02a52c80() & 0xffffu;
  FUN_02a490d0(record, static_cast<std::uint32_t>(kRecordBytes), selector);
  StoreField(record, kField0, id);
  StoreField(record, kField1, arg1);
  StoreField(record, kField2, arg2);
  FUN_02a49100(record);
}
}  // namespace NRadEngine
