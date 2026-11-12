#include "core/cargo_limits.h"

#include <algorithm>

namespace nevr_cargo {

namespace {

Limits Refuse(const char* reason) {
  Limits l;
  l.ok = false;
  l.reason = reason;
  return l;
}

}  // namespace

Limits ComputeLimits(const DeviceFacts& facts, std::uint64_t entry_bytes) {
  // A fact the device did not give: name it and stop. Nothing is guessed in its place.
  if (!facts.have_memory) return Refuse("no_memory_fact");
  if (!facts.have_free) return Refuse("no_free_space_fact");
  if (!facts.have_fds) return Refuse("no_descriptor_fact");
  if (entry_bytes == 0) return Refuse("no_entry_size");
  if (facts.memory_available == 0) return Refuse("no_memory");
  if (facts.free_bytes == 0) return Refuse("no_free_space");
  // Descriptors the process may still open; a count above the limit (the limit was lowered under it) leaves none.
  const std::uint64_t free_fds = facts.fd_soft_limit > facts.fds_open ? facts.fd_soft_limit - facts.fds_open : 0;

  Limits l;
  l.manifest_text_bytes = facts.memory_available / (kMemoryShareForManifest * (2 + kJsonExpansion));
  l.max_entries = facts.memory_available / kMemoryShareForEntries / entry_bytes;
  l.max_object_bytes = std::min<std::uint64_t>(kFormatMaxObjectBytes, facts.memory_available / kMemoryShareForObject);
  l.storage_budget = facts.free_bytes - facts.free_bytes / kStorageShareKept;
  l.store_file_bytes = std::min<std::uint64_t>(kFormatMaxStoreBytes, facts.free_bytes);
  l.max_store_files = free_fds / kDescriptorShareForStore;
  l.index_bytes = kIndexBytesFor(l.max_entries);
  if (l.max_store_files == 0) return Refuse("no_descriptors");
  if (l.manifest_text_bytes == 0 || l.max_entries == 0 || l.max_object_bytes == 0 || l.storage_budget == 0) {
    return Refuse(l.storage_budget == 0 ? "no_free_space" : "no_memory");
  }
  l.ok = true;
  return l;
}

}  // namespace nevr_cargo
