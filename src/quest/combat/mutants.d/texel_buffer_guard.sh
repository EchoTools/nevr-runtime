# texel_buffer_guard: each rule of src/quest/combat/texel_buffer_guard.cpp, texel_buffer_guard_rules.h and the device-function requests of
# src/quest/sentinel/vk_ext_window.cpp removed or bent once; src/quest/tests/texel_buffer_guard_test.cpp must fail on every one. Sourced by
# ../mutants.sh, which defines `runm <name> <file> <sed> [<build dir> <test target>]`.
F=src/quest/combat/texel_buffer_guard.cpp
H=src/quest/combat/texel_buffer_guard.h
R=src/quest/combat/texel_buffer_guard_rules.h
W=src/quest/sentinel/vk_ext_window.cpp
# the rules: only uniform texel bindings with a descriptor, a binding-flags layout left alone, the sizes
runm tbg_type_widened     $R "s/    if (b.descriptorType != vk::kDescriptorTypeUniformTexelBuffer || b.descriptorCount == 0) continue;/    if (b.descriptorType < vk::kDescriptorTypeUniformTexelBuffer || b.descriptorCount == 0) continue;/"
runm tbg_zero_count       $R "s/    if (b.descriptorType != vk::kDescriptorTypeUniformTexelBuffer || b.descriptorCount == 0) continue;/    if (b.descriptorType != vk::kDescriptorTypeUniformTexelBuffer) continue;/"
runm tbg_flags_ignored    $R "s/    if (p->sType == vk::kStructureTypeBindingFlagsCreateInfo) {/    if (false \&\& p->sType == vk::kStructureTypeBindingFlagsCreateInfo) {/"
runm tbg_too_many_kept    $R "s/    if (out.n == kMaxPicks || b.descriptorCount > kMaxDescriptors) {/    if (b.descriptorCount > kMaxDescriptors) {/"
runm tbg_wide_kept        $R "s/    if (out.n == kMaxPicks || b.descriptorCount > kMaxDescriptors) {/    if (out.n == kMaxPicks) {/"
runm tbg_none_stored      $R "s/        if (picks.verdict == Verdict::kNone) return Put::kNotStored;//"
runm tbg_reuse_kept       $R "0,/      if (s.handle == handle) {\$/s//      if (s.handle == handle \&\& picks.verdict != Verdict::kNone) {/"
runm tbg_no_capacity      $R "s/        if (size_ == kTableCapacity) return Put::kFull;//"
runm tbg_capacity         $R "s/kTableCapacity = 1024;/kTableCapacity = 1023;/"
runm tbg_memory_coherent  $R "s/  const std::uint32_t want = vk::kMemoryPropertyHostVisible | vk::kMemoryPropertyHostCoherent;/  const std::uint32_t want = vk::kMemoryPropertyHostVisible;/"
runm tbg_memory_bits      $R "s/    if ((typeBits & (1U << i)) != 0 && /    if ((typeBits | (1U << i)) != 0 \&\& /"
# the allocate wrapper: the driver first, its result untouched, a write only after success
runm tbg_result_check     $F "s/  if (result != vk::kSuccess || info == nullptr || sets == nullptr || info->pSetLayouts == nullptr ||/  if (info == nullptr || sets == nullptr || info->pSetLayouts == nullptr ||/"
runm tbg_result_swallowed $F "s/  const vk::Result result = real(device, info, sets);/  vk::Result result = real(device, info, sets);\n  if (result != vk::kSuccess) result = vk::kSuccess;/"
runm tbg_update_removed   $F "s/    if (pending != 0) g_update(device, pending, writes, 0, nullptr);/    (void)device;/"
runm tbg_update_first     $F "s/  const vk::Result result = real(device, info, sets);/  if (info != nullptr \&\& sets != nullptr \&\& info->pSetLayouts != nullptr) WriteNewSets(device, *info, sets);\n  const vk::Result result = real(device, info, sets);/"
runm tbg_skip_unchecked   $F "s/    if (picks.verdict != Verdict::kPicked) {/    if (false) {/"
runm tbg_other_device     $F "s/    if (device != g_device) {/    if (false) {/"
runm tbg_batch_unflushed  $F "s/      if (pending == kWriteBatch) flush();/      if (pending == kWriteBatch) pending = 0;/"
runm tbg_array_element    $F "s/      w.dstArrayElement = 0;/      w.dstArrayElement = 1;/"
runm tbg_count_one        $F "s/      w.descriptorCount = picks.picks\[k\].count;/      w.descriptorCount = 1;/"
runm tbg_batch_size       $H "s/kWriteBatch = 32;/kWriteBatch = 31;/"
# the layout wrapper: record only a layout the driver made
runm tbg_layout_unchecked $F "s/  if (result != vk::kSuccess || info == nullptr || layout == nullptr || g_viewState.load(std::memory_order_acquire) == kOff) return result;/  if (info == nullptr || layout == nullptr || g_viewState.load(std::memory_order_acquire) == kOff) return result;/"
runm tbg_off_records      $F "s/  if (result != vk::kSuccess || info == nullptr || layout == nullptr || g_viewState.load(std::memory_order_acquire) == kOff) return result;/  if (result != vk::kSuccess || info == nullptr || layout == nullptr) return result;/"
# the zeroed view: the buffer's shape, zeroed once, off for the process on a failure
runm tbg_no_memset        $F "s/  std::memset(mapped, 0, static_cast<std::size_t>(needs.size));/  (void)mapped;/"
runm tbg_format           $F "s/  viewInfo.format = vk::kFormatR32G32B32A32Sfloat;/  viewInfo.format = 0;/"
runm tbg_buffer_size      $F "s/  bufferInfo.size = kZeroBufferBytes;/  bufferInfo.size = kZeroBufferBytes \/ 2;/"
runm tbg_usage            $F "s/  bufferInfo.usage = vk::kBufferUsageUniformTexelBuffer;/  bufferInfo.usage = 0x10;/"
runm tbg_physical_any     $F "s/  if (r != vk::kSuccess || physicalCount != 1) return MadeFailed(\"physical_device\", r, kOffMsg);/  if (r != vk::kSuccess || physicalCount == 0) return MadeFailed(\"physical_device\", r, kOffMsg);/"
runm tbg_bind_unchecked   $F "s/  if (r != vk::kSuccess) return MadeFailed(\"bind_memory\", r, kOffMsg);//"
runm tbg_retry_after_fail $F "s/  g_viewState.store(made ? kMade : kOff, std::memory_order_release);/  g_viewState.store(made ? kMade : kNotMade, std::memory_order_release);/"
# the early step: the key, the two requests
runm tbg_key_ignored      $F "s/  if (!on) {/  if (!on \&\& false) {/"
runm tbg_one_request      $F "s/          : window::RequestDeviceFunction(kAllocateName, /          : window::RequestDeviceFunction(kCreateLayoutName, /"
runm tbg_refusal_info     $F "s/LogLevel::kError, \"combat_texel_buffer_guard_install\"/LogLevel::kInfo, \"combat_texel_buffer_guard_install\"/"
# the lines
runm tbg_first_every      $F "s/  if (setsWritten != 0 && !g_firstSaid.exchange(true, std::memory_order_acq_rel)) {/  if (setsWritten != 0) {/"
runm tbg_count_every      $F "s/  if ((sets & (sets - 1)) != 0) return;//"
runm tbg_full_every       $F "s/    if (!g_fullSaid.exchange(true, std::memory_order_acq_rel)) {/    if (true) {/"
runm tbg_end_wrapped      $F "s/  const bool wrapped = g_realCreateLayout.load(std::memory_order_acquire) != nullptr && g_realAllocate.load(std::memory_order_acquire) != nullptr;/  const bool wrapped = true;/"
runm tbg_end_always       $F "s/  if (!g_requested.load(std::memory_order_acquire)) return;//"
# vk_ext_window's device-function requests (the window owns the lookup; the guard is its input)
runm tbg_win_always_wrap  $W "s/  return requests != 0 ? reinterpret_cast<VoidFn>(&WrappedGetDeviceProcAddr) : found;/  return reinterpret_cast<VoidFn>(\&WrappedGetDeviceProcAddr);/"
runm tbg_win_never_wrap   $W "s/  return requests != 0 ? reinterpret_cast<VoidFn>(&WrappedGetDeviceProcAddr) : found;/  return requests > 99 ? reinterpret_cast<VoidFn>(\&WrappedGetDeviceProcAddr) : found;/"
runm tbg_win_no_publish   $W "s/    r.realOut->store(reinterpret_cast<void\*>(found), std::memory_order_release);/    (void)r.realOut;/"
runm tbg_win_no_seal      $W "s/  if (g_sealed.load(std::memory_order_acquire)) return RequestStatus::kSealed;//"
runm tbg_win_prefix       $W "s/    if (std::strcmp(name, r.name) != 0) continue;/    if (std::strncmp(name, r.name, std::strlen(r.name)) != 0) continue;/" quest-host-hooks vk_ext_window_test
# the same window rules against the window's own test
runm tbg_win_hooks_no_publish $W "s/    r.realOut->store(reinterpret_cast<void\*>(found), std::memory_order_release);/    (void)r.realOut;/" quest-host-hooks vk_ext_window_test
runm tbg_win_hooks_no_seal    $W "s/  if (g_sealed.load(std::memory_order_acquire)) return RequestStatus::kSealed;//" quest-host-hooks vk_ext_window_test
runm tbg_win_hooks_full       $W "s/  if (count == kMaxDeviceRequests) return RequestStatus::kFull;/  if (count > kMaxDeviceRequests) return RequestStatus::kFull;/" quest-host-hooks vk_ext_window_test
runm tbg_win_hooks_duplicate  $W "s/    return r.wrapper == wrapper \&\& r.realOut == realOut ? RequestStatus::kOk : RequestStatus::kDuplicate;/    return RequestStatus::kOk;/" quest-host-hooks vk_ext_window_test
runm tbg_win_hooks_same_dup   $W "s/    return r.wrapper == wrapper \&\& r.realOut == realOut ? RequestStatus::kOk : RequestStatus::kDuplicate;/    return RequestStatus::kDuplicate;/" quest-host-hooks vk_ext_window_test
runm tbg_win_hooks_one_field  $W "s/    return r.wrapper == wrapper \&\& r.realOut == realOut ? RequestStatus::kOk : RequestStatus::kDuplicate;/    return r.wrapper == wrapper ? RequestStatus::kOk : RequestStatus::kDuplicate;/" quest-host-hooks vk_ext_window_test
runm tbg_win_hooks_null_real  $W "0,/  if (found == nullptr || name == nullptr) return found;\$/s//  if (name == nullptr) return found;/" quest-host-hooks vk_ext_window_test
runm tbg_win_hooks_said_every $W "s/    if (!r.said.exchange(true, std::memory_order_acq_rel)) {/    if (true) {/" quest-host-hooks vk_ext_window_test
runm tbg_win_hooks_instance   $W "s/  g_lookupInstance.store(instance, std::memory_order_release);/  (void)instance;/" quest-host-hooks vk_ext_window_test
