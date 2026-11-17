// PLAN-v7 H-CTRL (#2242): the chain's decisions, pure. See gear_controller.h for what is here and what is not.

#include "runtime/combat/gear_controller.h"

#include <cstring>

namespace nevr_combat::gear_controller {

namespace {

// The cell's words are read and written as bytes: the cell is the game's memory, and a caller's buffer in tests.
std::uint64_t ReadU64(const std::uint8_t* cell, std::size_t offset) {
  std::uint64_t value = 0;
  std::memcpy(&value, cell + offset, sizeof value);
  return value;
}

std::uint32_t ReadU32(const std::uint8_t* cell, std::size_t offset) {
  std::uint32_t value = 0;
  std::memcpy(&value, cell + offset, sizeof value);
  return value;
}

void WriteU64(std::uint8_t* cell, std::size_t offset, std::uint64_t value) {
  std::memcpy(cell + offset, &value, sizeof value);
}

void WriteU32(std::uint8_t* cell, std::size_t offset, std::uint32_t value) {
  std::memcpy(cell + offset, &value, sizeof value);
}

}  // namespace

bool ContinuesOn(std::uint64_t port) {
  return port == kDefaultPort || port == kOutPort;
}

std::int64_t Successor(std::size_t index, std::uint64_t port) {
  if (index + 1 >= kChainLength || !ContinuesOn(port)) return kEnd;
  return kChain[index + 1];
}

TailStep ChainTail(std::uint64_t port, std::uint64_t selected, std::uint64_t queried, std::uint32_t reruns) {
  if (!ContinuesOn(port)) return TailStep{kEnd, reruns};
  if (selected == 0) return TailStep{kEnd, reruns};
  if (selected == queried) return TailStep{kEnd, 0};
  // Saturates just above kMaxReruns: his `lock inc` has no bound, so after 2^32 re-runs the count wraps to 0 and the chain
  // would query again for a selection that has been enabled for a long time.
  const std::uint32_t counted = reruns <= kMaxReruns ? reruns + 1 : kMaxReruns + 1;
  if (counted > kMaxReruns) return TailStep{kEnd, counted};
  return TailStep{kQueryNode, counted};
}

std::int64_t RunTail(std::uint8_t* cell, std::uint64_t port) {
  const TailStep step = ChainTail(port, ReadU64(cell, kSelectedOffset), ReadU64(cell, kQueriedOffset),
                                  ReadU32(cell, kRerunsOffset));
  WriteU32(cell, kRerunsOffset, step.reruns);
  return step.next;
}

void RecordQuery(std::uint8_t* cell, std::uint64_t queried, std::uintptr_t writer) {
  WriteU64(cell, kQueriedOffset, queried);
  WriteU64(cell, kWriterOffset, static_cast<std::uint64_t>(writer));
}

}  // namespace nevr_combat::gear_controller
