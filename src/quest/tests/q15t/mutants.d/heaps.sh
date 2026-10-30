# heaps: each mutant drops one rule; tests/heaps_test.cpp must fail on it
runm heaps heaps_no_stock_check     src/heaps.cpp 's/if (!ok || rodata != pin.stock) {/if (false) {/'
runm heaps heaps_no_reader_check    src/heaps.cpp 's/if (!ok || reader != pin.reader_word) {/if (false) {/'
runm heaps heaps_quest2_ignored     src/heaps.cpp 's/return std::strcmp(device, "hollywood") == 0 ? kSystemQuest2 : kPins\[row\].target;/return (void)kSystemQuest2, (void)device, kPins[row].target;/'
runm heaps heaps_quest2_inverted    src/heaps.cpp 's/std::strcmp(device, "hollywood") == 0 ? kSystemQuest2/std::strcmp(device, "hollywood") != 0 ? kSystemQuest2/'
runm heaps heaps_value_stock        src/heaps.cpp 's/g_value\[row\] = value;/g_value[row] = pin.stock;/'
runm heaps heaps_no_expected_slot   src/heaps.cpp 's/SwapWord(base + pin.got, base + pin.ro, /SwapWord(base + pin.got, *reinterpret_cast<std::uintptr_t*>(base + pin.got), /'
runm heaps heaps_stop_on_first      src/heaps.cpp 's/^      ++g_refused;$/      ++g_refused; break;/'
runm heaps heaps_off_ignored        src/heaps.cpp 's/if (ModuleSwitchPresent("heaps.off")) {/if (false) {/'
runm heaps heaps_memsys_off_ignored src/heaps.cpp 's/if (row == kSystemRow \&\& memsys_off) {/if (false \&\& memsys_off) {/'
runm heaps heaps_memsys_off_all     src/heaps.cpp 's/if (row == kSystemRow \&\& memsys_off) {/if (memsys_off) {/'
runm heaps heaps_no_base_check      src/heaps.cpp 's/^  if (base == 0) {$/  if (false) {/'
runm heaps heaps_slot_r08           src/heaps.cpp 's/0x3710188, 0x2b9ca98/0x3710180, 0x2b9ca98/'
runm heaps heaps_ro_r09             src/heaps.cpp 's/0x3715198, 0x2b9e1c0/0x3715198, 0x2b9e1c8/'
runm heaps heaps_target_r06         src/heaps.cpp 's/206 \* kMiB, 320 \* kMiB/206 * kMiB, 300 * kMiB/'
runm heaps heaps_target_r09         src/heaps.cpp 's/40 \* kMiB, 128 \* kMiB/40 * kMiB, 96 * kMiB/'
runm heaps heaps_reader_r09         src/heaps.cpp 's/0x11ffdec, kLdrX0FromX8/0x11ffdec, kLdrX1FromX8/'
runm heaps heaps_observed_zero      src/heaps.cpp 's/ok ? seen : 0, value, device);/seen * 0, value, device);/'
runm heaps heaps_refusal_quiet      src/heaps.cpp 's/LogRow(NEVR_LOG_ERROR, "heaps row refused"/LogRow(NEVR_LOG_INFO, "heaps row refused"/'
runm heaps heaps_stock_warn_quiet   src/heaps.cpp 's/LogRow(NEVR_LOG_WARNING, "heaps row refused", row, "rodata word/LogRow(NEVR_LOG_INFO, "heaps row refused", row, "rodata word/'
runm heaps heaps_status_lost        src/heaps.cpp 's/^        result.status = o.status;$/        result.status = SwapStatus::kOk;/'
# the shared swap rules, judged by the heaps tests
runm heaps heaps_swap_no_exec_check src/got_swap.cpp "s/if (m.perms\[2\] == 'x') {/if (false) {/"
runm heaps heaps_swap_no_expected   src/got_swap.cpp "s/if (r.original != expected) {/if (false) {/;s/const bool swapped = word->compare_exchange_strong(want, replacement, std::memory_order_acq_rel);/word->store(replacement); const bool swapped = true; (void)want;/"
runm heaps heaps_swap_no_restore    src/got_swap.cpp "s/mprotect(reinterpret_cast<void\*>(base), page, PROT_READ) != 0/false/"
