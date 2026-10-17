# ADR 0008: the Quest process ends when the game's android_main returns

Status: accepted, implemented (`src/quest/sentinel/process_end_hook.{h,cpp}`, #621). On in every Quest build, with no feature
flag. Not yet observed on a headset: the "Prediction" section is what the first run must show.

## Context

The stock activity is a bare `NativeActivity`; the native-activity glue (`libr15.so`, thread entry containing `0xf79a4c`) calls
`android_main` (`0xf784c4`) through libr15's own PLT (`JUMP_SLOT 0x36eb4c8`) and ignores the result. `android_main` runs `Main`,
shuts the engine down, asks the activity to finish when `app->destroyRequested` (`+0x64`) is 0 (`ANativeActivity_finish`,
`0xf787b0`), pumps the looper until the destroy, and returns. Nothing on that path calls `exit`, so the process stays alive with
no activity, and our threads (token-auth poll, the auth worker, the bridge) keep running in it.

The stock engine cannot run a second `android_main` in the same process: `CFileTypes::InitializeDirectories` (`0x11eb0f0`) runs
once per process (its only caller is `CEngine::InitGlobals`, and the second run's log lacks its `Hardcoded project path` line), and
`NRadCrypt::Shutdown` (`0xf9cd60`) calls `OPENSSL_cleanup` (`0xf9cd98`), after which `X509_STORE_new` fails. Every relaunch into
the leftover process therefore fails its initialisation (`Failed to find package manifest file //android/manifests/...`) and
finishes its own activity, until a force-stop. The first launch of a fresh install fails the same way for another reason (the
storage permission is not granted yet, so the package manifest cannot be read); that is a separate issue.

## Decision

One GOT hook on libr15's `android_main` slot (`LibR15AndroidMain`, build-ID pinned like the others). The handler calls the original;
when it returns, for any reason:

1. logs `{"event":"android_main_returned","action":"ending_process","exit_code":0}`;
2. runs the before-exit callback on a helper thread and waits for it at most 500 ms. The integration registers
   `ReportAndroidMainReturned` there: it reports the self-check `android_main_returned` and flushes the unit once, so the result
   reaches the remote log when the login is up (the sentinel line is the record when it is not, or when the flush is late);
3. ends the process with `_exit(0)`, as libr15's own failure paths end it with `exit(0)` (`0x1a6f124`, `0x1a70d08`). `_exit` runs no
   static destructors: our threads and leaked objects are live.

A new process is the only reset that works: stopping our threads would not change the engine's once-per-process state, and
clearing the engine's destroy flag or its statics would re-run initialisation in a used process, undoing `OPENSSL_cleanup` is not
possible. Steps 1 to 3 are reached from the handler only through function pointers, so no personality-bearing frame is live
across game code (`callback_thunk.h`, rules 1 and 5; the frame sensor follows direct edges only).

## What our Quest code writes, and what the cut does to it

| File | Writer | Write |
| --- | --- | --- |
| the credential cache (`.credentials.json` in the app files dir) | `FileCredentialStore::Save`, `auth/file_store.cpp` | `AtomicWrite` |
| `device_login.txt` | `FileLinkPresenter::Present`, `auth/file_store.cpp` (`Clear` unlinks it) | `AtomicWrite` |
| the hardware dump report (off by default) | `quest/diag/hwdump_run.cpp` | `AtomicWrite` |
| the sentinel's on-disk log | `sentinel/sentinel_log.cpp` | `O_APPEND` lines; rotation by `rename` |
| a crash dump and its `.maps` copy | `sentinel/sentinel.cpp` `SnapshotMaps`, only inside the breakpad callback | `O_TRUNC` write, not reached by this exit |

`AtomicWrite` (`auth/atomic_write.cpp`) writes a fresh temp file (`O_EXCL`, `O_NOFOLLOW`, mode 0600), `fsync`s it, renames it over the
target and `fsync`s the directory; a stale temp from a cut is unlinked by the next write. A process ended mid-write therefore leaves
the previous file whole; the newest token may be lost, exactly as in a crash, and is asked for again at the next sign-in. The log is
append-only diagnostics, not sign-in state: a cut can truncate its last line, and logcat has the same line. So the exit waits for no
in-flight write. `process_end_hook_test.cpp` ends a real process 60 times under a writer saving 192 KiB payloads the way the
credential cache is saved, and requires a whole file every time.

## Prediction

First launch of a fresh install (storage permission not yet granted), pid A: the logs are today's up to `APP_CMD_DESTROY`, with
`install_process_end` among the constructor steps (`steps_ok` 19 where it was 18) and the sign-in lines saying `code shown`; then one
`NEVR-Sentinel` `android_main_returned` line, the process dies, and no pid A line follows. The relaunch after the grant starts a new
pid B with a `sentinel_ctor`, the `Hardcoded project path (rad15 => rad15) for android` warning, no `X509_STORE_new failed`, no
`Initialization Error`, no `wm_finish_activity ... app-request`, and `Displayed ... MainActivity`. A relaunch with no `Start proc`,
or a pid A line after `android_main_returned`, falsifies it.

## Not claimed

That the first launch works (it needs the permission), that no other path keeps a process alive (four exit and kill sites in libr15
are not traced to their callers), and what the platform does with an activity-less process.
