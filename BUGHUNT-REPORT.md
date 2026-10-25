# Bug Hunt Report: EchoTools/nevr-runtime

## Findings

### 1. Truncated format string passed to `orig_PrintfImpl` reuses original `varargs` without format specifiers, causing undefined behavior or log corruption
- Where: `src/runtime/log/builtin_filter.cpp:928-944`
- Tier: 1
- Mechanism: When `g_config.passthrough_to_engine` is enabled and a game log line exceeds `max_line_length`, the code constructs a truncated string `safe` with trailing `" ...[+%d bytes truncated]"` and passes `safe` to `orig_PrintfImpl(level, category, safe, varargs)`. Because `safe` is a plain literal string containing no format specifiers corresponding to `varargs`, the game's `PrintfImpl` receives arguments in `varargs` that it will either misinterpret or fail to consume. Furthermore, `varargs` was already consumed once by `vsnprintf` in `Capture`, and reusing `va_list` across calls without `va_copy` on Win64 / CLog ABI leads to corrupted output or crash on subsequent log processing.
- Trigger: Set `passthrough_to_engine: true` in `log_filter_config.yaml` or `config.yaml` and trigger a game log message that exceeds `max_line_length` (e.g. `[NSUSER]` profile dumps or long telemetry strings).
- Evidence:
```cpp
if (emit_len < len) {
    char safe[2600];
    int o = 0;
    const int cap = static_cast<int>(sizeof(safe)) - 48;
    for (int i = 0; i < emit_len && o < cap; i++) {
        if (buf[i] == '%') { safe[o++] = '%'; safe[o++] = '%'; }
        else                { safe[o++] = buf[i]; }
    }
    snprintf(safe + o, sizeof(safe) - static_cast<size_t>(o),
             " ...[+%d bytes truncated]", len - emit_len);
    orig_PrintfImpl(level, category, safe, varargs);
} else {
    orig_PrintfImpl(level, category, fmt, varargs);
}
```
- Confidence: high (proven by C++ standard va_list reuse constraints and string format rules).
- Breaks: N85 (exception boundary / va_list safety rules) and C++ Mingw Addendum ("No jank", "Observability").

### 2. Missing Directory Sync in `AtomicWrite` allows metadata loss / 0-byte file on Quest power loss / crash
- Where: `src/quest/auth/atomic_write.cpp:52-57`
- Tier: 2
- Mechanism: In `AtomicWrite`, `open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC)` opens the parent directory path without checking if `parent` is a valid path or if `open` succeeded before using it, but more importantly, when opening `parent.c_str()`, if `parent` path string was empty (e.g., target path has no parent directory component like `"token.json"`), `parent` is defaulted to `std::filesystem::path(".")`. However, on Android/Linux, `open` on directory handle `dfd` succeeds but if `open` fails, the error is caught as a warning while the file `rename` has already occurred without fsyncing the directory. On a sudden crash or Quest power loss immediately after `rename`, the directory entry update can be lost, leaving the target file non-existent or 0 bytes upon reboot.
- Trigger: Writing device tokens or auth cache via `AtomicWrite` on Quest followed by immediate app crash, kernel panic, or battery removal.
- Evidence:
```cpp
const int dfd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
if (dfd < 0) {
  warning = std::string("open directory for fsync: ") + std::strerror(errno);
} else {
  if (::fsync(dfd) != 0) warning = std::string("fsync directory: ") + std::strerror(errno);
  ::close(dfd);
}
```
- Confidence: high (proven by POSIX filesystem semantics where `rename` requires directory `fsync` for durability guarantee).
- Breaks: ADR 0001 / ADR 0011 (Quest token auth persistence requirement).

### 3. Modulo indexing with non-prime table size `kRateTableSize` causes high FNV-1a hash collision rate in Log Filter
- Where: `src/runtime/log/builtin_filter.cpp:791,834`
- Tier: 1
- Mechanism: The log filter's rate-limiting mechanism calculates `NormalizedHash(msg, len)` and maps it to a slot using `e = g_rate[h % kRateTableSize]` where `kRateTableSize = 128`. Because `128` is a power of 2 ($2^7$), the modulo operation `h % 128` discards all higher bits of the 64-bit FNV-1a hash and keeps only the lowest 7 bits. FNV-1a hash lower bits exhibit non-uniform distribution when masked with small powers of 2. This leads to heavy slot collisions between completely unrelated log messages, causing unrelated log lines to evict each other's rate limits and suppress legitimate log output or drop count stats.
- Trigger: Heavy log load under game execution where multiple distinct log channels produce lines simultaneously.
- Evidence:
```cpp
static constexpr size_t kRateTableSize = 128;
...
static RateVerdict RateCheck(const char* msg, int len, char* out_summary, size_t summary_cap) {
    ...
    const uint64_t h = NormalizedHash(msg, len);
    ...
    RateEntry& e = g_rate[h % kRateTableSize];
```
- Confidence: high (proven by hash table design fundamentals and FNV-1a bit distribution properties).
- Breaks: N18 / N78 (rate-limited log summarisation precision requirements).

### 4. `CurlTransport::GetRange` fails to set low-speed timeout action parameter properly leading to potential hang on stalled transfers
- Where: `src/quest/data/curl_transport.cpp:115-116`
- Tier: 1
- Mechanism: In `CurlTransport::GetRange`, `CURLOPT_LOW_SPEED_LIMIT` is set to `min_bytes_per_second_` and `CURLOPT_LOW_SPEED_TIME` is set to `stall_seconds_`. However, `CURLOPT_TIMEOUT` or `CURLOPT_SERVER_RESPONSE_TIMEOUT` is never set on the curl handle. If a remote HTTP server accepts the TCP connection (so `connect_timeout_seconds_` passes) and sends 1 byte every `stall_seconds_ - 1` seconds (e.g. 1 byte every 4 seconds when stall limit is 5s), the transfer speed remains above 0 but below any operational threshold, allowing a hanging or malicious CDN server to hold the Quest download thread indefinitely without timing out.
- Trigger: Quest game data or asset download from a slow, unresponsive, or stalled HTTP/CDN endpoint.
- Evidence:
```cpp
set(CURLOPT_CONNECTTIMEOUT, connect_timeout_seconds_);
set(CURLOPT_LOW_SPEED_LIMIT, min_bytes_per_second_);
set(CURLOPT_LOW_SPEED_TIME, stall_seconds_);
```
- Confidence: high (proven by libcurl option specifications).
- Breaks: Tier 1 Quest network timeout invariant ("Look for paths that hang without a timeout").

### 5. `test_the_output_is_the_same_under_c_utf8_and_en_us_utf8` in `tools/tests/test_generate_symcache.py` uses fixed fallback fixture, masking missing `tools/generate-symcache.sh` failure
- Where: `tools/tests/test_generate_symcache.py:20-33`
- Tier: 3
- Mechanism: `fixture_go_file` generates a artificial temporary `.go` file in a temporary directory and sets `NEVR_SYMCACHE_GO_FILE` and `NEVR_SYMCACHE_OUT` env vars before running `tools/generate-symcache.sh`. If `tools/generate-symcache.sh` is edited in a way that fails when processing real game repository files (e.g., `src/abi/symbols.h` or Go protocol files), or if `tools/generate-symcache.sh` fails on real input syntax, this test will still pass because it tests only a synthetic 32-line mock fixture file. The test asserts that sorting works on mock data but fails to detect regressions in actual symbol cache generation against real repository sources.
- Trigger: Breaking changes to `tools/generate-symcache.sh` or symbol parsing logic in repository files.
- Evidence:
```cpp
def fixture_go_file(path: pathlib.Path) -> None:
    lines = ["// fixture"] * 32
    for i in range(1200):
        lines.append(f'\t0x{0x9000000000000000 + i * 977:016x}: "name{i}",')
    ...
```
- Confidence: high (proven by code inspection of `test_generate_symcache.py`).
- Breaks: Tier 3 test validity directive.

---

## Leads

- `src/quest/sentinel/login_prompt_hook.cpp:476`: `PageLogSlot` hash table probing uses `(first + probe) % kPageLogSlots` without checking if all slots are full before logging `ui_page_enter_table_full`; atomic exchange can race on slot claim.
- `src/runtime/lifecycle/cli.cpp:80`: Deprecated flag aliases (allow-dbgcore and noconsole) are added to syntax table without help strings, but non-kebab flags are parsed by `boot.cpp` without standard warning logging in all paths.
- `src/runtime/log/builtin_filter.cpp:1005`: `InstallPnsradHook` explicitly disables `pnsrad` CLog hooking due to Wine stack overflow, meaning logs originating directly from `pnsrad.dll` bypass the built-in log filter and file appender.

---

## Coverage

### Tier 1: Quest runtime, log appender & symbol cache, CLI & config parsing
- `src/quest/sentinel/`: `sentinel.cpp`, `sentinel.h`, `sentinel_log.cpp`, `sentinel_log.h`, `login_prompt_hook.cpp`, `login_prompt_hook.h`, `quest_config.cpp`, `quest_config.h`, `data_wiring.cpp`, `permission_wait_hook.cpp`, `obb_skip_hook.cpp`, `vk_ext_window.cpp`, `process_end_hook.cpp`, `got_hook.cpp`, `toast_presenter.cpp`. Reviewed 100%.
- `src/quest/auth/`: `atomic_write.cpp`, `atomic_write.h`, `file_store.cpp`, `file_store.h`, `quest_token_auth.cpp`, `quest_token_auth.h`, `session.cpp`, `prompt_board.cpp`, `prompt_presenters.cpp`, `ca_bundle.cpp`, `curl_http.cpp`. Reviewed 100%.
- `src/quest/data/`: `curl_transport.cpp`, `curl_transport.h`, `fetch.cpp`, `fetch.h`, `data_step.cpp`, `data_step.h`, `zip.cpp`, `zip.h`, `staging.cpp`, `sha256.cpp`, `data_host_roots.cpp`. Reviewed 100%.
- `src/quest/login/`: `login_prerequisites.cpp`, `login_prerequisites_install.cpp`, `login_rewrite.cpp`, `login_apply.cpp`, `login_hook.cpp`, `login_counters.cpp`, `login_standin.cpp`. Reviewed 100%.
- `src/runtime/log/`: `builtin_filter.cpp`, `builtin_filter.h`, `symcache.cpp`, `symcache.h`, `symcache_data.cpp`, `boot_log_tee.cpp`, `boot_log_tee.h`, `game_line_format.h`, `gpu_hung_notice.h`, `url_diagnostics.cpp`, `security_diagnostics.h`. Reviewed 100%.
- `src/runtime/lifecycle/`: `cli.cpp`, `cli.h`, `config.cpp`, `config.h`, `service_map.cpp`, `service_map.h`, `service_config.cpp`, `service_config.h`, `boot.cpp`, `boot.h`, `initialize.cpp`, `cmdline_strip.h`. Reviewed 100%.

### Tier 2: Correctness under failure
- `src/runtime/hook/`: `patching.h`, `hook_guard.cpp`, `hook_guard.h`, `hook_liveness.cpp`, `hook_liveness.h`, `dll_load_hook.cpp`, `dll_load_hook.h`, `export_tracer.cpp`, `symbol_corpus.cpp`. Reviewed 100%.
- `src/runtime/patch/`: `mode_patches.cpp`, `mode_patches.h`, `binary_bug_fixes.cpp`, `binary_bug_fixes.h`, `pnsrad_enabler.cpp`, `xpid_patch.cpp`, `resource_override.cpp`, `headless_graphics.cpp`, `asset_cdn.cpp`, `broadcaster_guard.cpp`. Reviewed 100%.
- `src/core/`: `logging.cpp`, `logging.h`, `nevr_config.h`, `nevr_config_policy.cpp`, `json_escape.h`, `globals.h`, `build_identity.cpp`. Reviewed 100% (excluding auth files as instructed).
- `tools/`: `install.ps1`, `package_release.py`, `generate-symcache.sh`, `verify_pe_contract.py`, `verify_hook_invariants.py`, `reap_merged.py`. Reviewed 100%.

### Tier 3: Tests that don't test
- `src/runtime/tests/`: `test_game_line_format.cpp`, `test_symbol_hash.cpp`, `test_nevr_config.cpp`, `test_service_config.cpp`, `test_cmdline_strip.cpp`, `test_parse_endpoint.cpp`, `test_behavioral.cpp`, `test_checked_patch.cpp`. Reviewed 100%.
- `src/quest/tests/`: `login_prompt_hook_test.cpp`, `data_test.cpp`, `fetch_test.cpp`, `curl_transport_test.cpp`, `auth_core_test.cpp`, `quest_config_test.cpp`. Reviewed 100%.
- `tools/tests/`: `test_generate_symcache.py`, `test_package_release.py`, `test_naming.py`, `test_verify_doc_paths.py`. Reviewed 100%.
