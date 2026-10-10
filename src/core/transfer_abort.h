#pragma once

#include <atomic>

namespace nevr {

/// Body of a libcurl CURLOPT_XFERINFOFUNCTION: a non-zero return aborts the transfer
/// (CURLE_ABORTED_BY_CALLBACK). curl calls it about once a second and on every chunk, so a transfer
/// that is stalled or slow stops within that interval of the flag being set instead of running to
/// CURLOPT_TIMEOUT. `requested` is the CURLOPT_XFERINFODATA pointer.
inline int AbortTransferWhenRequested(const std::atomic<bool>* requested) {
  return (requested != nullptr && requested->load(std::memory_order_acquire)) ? 1 : 0;
}

}  // namespace nevr
