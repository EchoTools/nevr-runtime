# Headset sheet: Quest texture quality

Headset proof for the config key `graphics.texture_quality`. Run it once on a Quest 2 and once on a Quest 3; fill the Quest 2 and Quest 3 columns with PASS or FAIL and paste the line you saw. A column left empty means that headset has not been run.

What this checks: the key changes the size of the buffer the game renders each eye into. The game creates that buffer once, at start-up, so every step is a fresh launch. The sizes are the community patcher's numbers, reported for a Quest 3; this build has not measured either headset. Step 5 reads the headset's own size so we have it.

The default is `stock`: the build changes nothing unless the file asks. The default becomes `high` only after a Quest 2 has passed step 9 (frame time, frame rate, temperature and memory at `high`); step 9 is the one that decides the flip. Any mistake in the key (a typo in the name or the value, a wrong case, a number, a list, a section of the wrong shape) also gives `stock`, never `high`, and the log names the key.

## Before you start

You need the APK from the build under test, adb over USB or Wi-Fi, and the game already signed in (this sheet does not test sign-in). Game files folder on the headset: `/sdcard/Android/data/com.readyatdawn.r15/files/`.

Two commands used in every step:

- Start fresh and clear logcat: `adb shell am force-stop com.readyatdawn.r15 && adb logcat -c`
- Start the game from the headset menu (or `adb shell monkey -p com.readyatdawn.r15 1`), wait until the main menu shows (about a minute), then read: `adb logcat -d | grep -F "Creating OVR swap chain"` and `adb logcat -d | grep -F texture_quality`

Pull the sentinel's own log when logcat has rolled over: `adb pull /sdcard/Android/data/com.readyatdawn.r15/files/nevr-sentinel.log .` and search it for `texture_quality` and `Creating OVR swap chain`. The swap-chain line is the game's own log line; the logcat tag the game writes it under is not known, so search the whole logcat, not one tag. If it is not in logcat, it may be in the game's own log file in the same files folder: `adb shell ls /sdcard/Android/data/com.readyatdawn.r15/files/` and search any `.log` or `.txt` there.

To set the level: write a `config.yaml` with exactly these two lines (one level word, lower case) and push it:

```
graphics:
  texture_quality: low
```

`adb push config.yaml /sdcard/Android/data/com.readyatdawn.r15/files/config.yaml`

If you already have a `config.yaml` there (sign-in keys), add the two lines to it instead of replacing it.

## Steps

| # | What you do | What you should see | Line to find | Quest 2 | Quest 3 |
| --- | --- | --- | --- | --- | --- |
| 1 | Default. Remove any `graphics:` section from `config.yaml` (or have no file). Force-stop, clear logcat, start. | Game reaches the main menu, the look the game had before this build. | sentinel: `"event":"texture_quality"` with `"result":"not_installed"`, `"quality":"stock"`, `"source":"default"`, `"why":"stock"`. Game: `Creating OVR swap chain with size <W> x <H> (2 slices, 2 sample[s])`; write <W> and <H> in the cell: this is the headset runtime's own size | | |
| 2 | Set `texture_quality: low`. Force-stop, clear logcat, start. | Main menu. | sentinel: `"result":"installed"`, `"quality":"low"`, `"source":"config"`, `"w":1680`, `"h":1760`. Game: `Creating OVR swap chain with size 1680 x 1760 (2 slices, 2 sample[s])` | | |
| 3 | Set `texture_quality: high`. Force-stop, clear logcat, start. | Main menu. Sharper than step 1. | sentinel: `"result":"installed"`, `"quality":"high"`, `"source":"config"`, `"w":3072`, `"h":3380`. Game: `Creating OVR swap chain with size 3072 x 3380 (2 slices, 2 sample[s])` | | |
| 4 | Set `texture_quality: ultra`. Force-stop, clear logcat, start. Watch for a crash or a black picture. | Main menu, and the game stays up for a minute. | sentinel: `"quality":"ultra"`, `"w":4096`, `"h":4506`. Game: `Creating OVR swap chain with size 4096 x 4506 (2 slices, 2 sample[s])` | | |
| 5 | Read the headset's own size. Set `texture_quality: high` (at `stock` no hook is installed and these counters do not exist), start, wait 90 seconds on the main menu, pull `nevr-sentinel.log`. | (nothing to look at) | two `hook_counter` lines, `"counter":"gfx_native_eye_width"` and `"counter":"gfx_native_eye_height"`, each with a `"value"`: write both numbers in the cell. They are what the headset runtime suggested before our override | | |
| 6 | Bad value. Set `texture_quality: Ultra` (capital U). Force-stop, clear logcat, start. | Main menu, same picture as step 1. | sentinel warning `config file key=graphics.texture_quality rejected reason=unknown_level value=Ultra (falls back to stock; allowed: stock, low, medium, high, very_high, ultra)`, then `"result":"not_installed"`, `"quality":"stock"`, `"source":"default"`, and the swap-chain size of step 1 | | |
| 7 | Not a word. Set `texture_quality: [high]` (a list). Force-stop, clear logcat, start. | Main menu, same picture as step 1. | sentinel warning `config file key=graphics.texture_quality rejected reason=not_a_word (falls back to stock; ...)`, then `"quality":"stock"`, and the swap-chain size of step 1 | | |
| 8 | Level change needs a restart; a misspelt key is named. (a) With the game running at `high`, edit `config.yaml` to `low` but do not restart. (b) Then force-stop, set `texture_qualty: high` (the key misspelt) under `graphics`, clear logcat, start. | (a) Nothing changes until the next launch. (b) Main menu, same picture as step 1. | (a) game still at 3072 x 3380 until force-stop and start, then 1680 x 1760. (b) sentinel warning `config file key=graphics rejected reason=unknown_key name=texture_qualty (ignored; the only key under graphics is texture_quality)`, then `"quality":"stock"` and the swap-chain size of step 1 | | |
| 9 | Quest 2 only: the measurement that flips the default. For each level in the table under "Step 9" below: set the level, force-stop, clear logcat, start, join a match or a lobby that is normally busy, and play two minutes while `adb logcat -v time > step9-<level>.log` runs in a second terminal. Then `adb shell dumpsys meminfo com.readyatdawn.r15`, write the `TOTAL PSS` line, and summarise the log (below). | Whether `high` holds frame rate, frame time, temperature and memory. | In the cell, for each level: the summary line, the `TOTAL PSS`, and whether the game stayed up for the whole two minutes. Then PASS or FAIL for `high` against the thresholds under "Step 9" | | |

### Step 9

Levels: `stock` and `high` are required (the pass decision compares them); `low`, `medium`, `very_high` and `ultra` use the same procedure and are recorded without a threshold. Each is a fresh launch and two minutes of play.

The numbers come from the runtime's once-a-second stats line in logcat. Its shape is the VrApi SDK's, not one verified on this APK: `FPS=<achieved>/<target>`, `Stale=<n>`, `App=<ms>`, `Free=<MB>`, `Temp=<battery>C/<sensor>C`. The tag it is written under is not known, so search the whole log for `FPS=`. A log with no such line fails the step: say so, and record the frame rate you saw and any overlay reading instead.

Summarise one level's log with this (`temp_start` and `temp_end` are the first `Temp=` value of the first and of the last stats line):

```
awk '/FPS=/ {
  n++
  if (match($0, /FPS=[0-9]+\/[0-9]+/)) { split(substr($0, RSTART + 4, RLENGTH - 4), f, "/"); if (f[1] == f[2]) ok++; if (minfps == "" || f[1] + 0 < minfps) minfps = f[1] + 0; target = f[2] + 0 }
  if (match($0, /Stale=[0-9]+/)) stale += substr($0, RSTART + 6, RLENGTH - 6)
  if (match($0, /Free=[0-9]+MB/)) { v = substr($0, RSTART + 5, RLENGTH - 7) + 0; if (minfree == "" || v < minfree) minfree = v }
  if (match($0, /Temp=[0-9.]+C/)) { t = substr($0, RSTART + 5, RLENGTH - 6) + 0; if (t0 == "") t0 = t; t1 = t }
  if (match($0, /App=[0-9.]+ms/)) app[++na] = substr($0, RSTART + 4, RLENGTH - 6) + 0
}
END {
  for (i = 2; i <= na; i++) { x = app[i]; j = i - 1; while (j >= 1 && app[j] > x) { app[j + 1] = app[j]; j-- } app[j + 1] = x }
  printf "lines=%d at_target=%d min_fps=%s target=%s stale_sum=%d min_free_mb=%s temp_start=%s temp_end=%s app_median_ms=%s\n", n, ok, minfps, target, stale, minfree, t0, t1, (na ? app[int((na + 1) / 2)] : "absent")
}' step9-high.log
```

Over two minutes expect about 120 stats lines. A summary with far fewer lines means the game was not in play for the window or logcat rolled over: repeat the level.

`high` passes step 9 when all of these hold in the same run, with `stock` measured on the same headset and the same session:

- `lines` is at least 100, and `at_target` is at least 95% of `lines` (frame rate at the headset's target rate for 95% of the seconds).
- `min_fps` is no lower than the target minus 10.
- `stale_sum` is no more than 5.
- `app_median_ms` is no more than 80% of the frame budget, which is 1000 divided by the target rate in ms (11.1 ms at 72, 8.9 ms at 90).
- `temp_end` is no more than 3.0 C above the `stock` run's `temp_end`, and no more than 45.0 C.
- `min_free_mb` is at least 300, and the `high` run's `TOTAL PSS` is no more than 600 MB above the `stock` run's.
- The game stays up for the whole two minutes (same process before and after: `adb shell pidof com.readyatdawn.r15` gives the same number at the start and at the end).

These thresholds are this sheet's decision rule for flipping the default, not measured platform limits; the results are the data, and any single failed line keeps the default at `stock`.

## Pass and fail

- A step passes when the sentinel line and the game's swap-chain line both show the numbers in the row.
- Fail: the game's swap-chain size differs from the level's size (the game reads the size somewhere else, or creates the swap chain before the hook installs); no `texture_quality` line at all; a crash or black picture at any level.
- Quest 2: the `high` size is well above that headset's native size (about 4.5 times the stock pixel count), so expect the frame rate at `high` and `ultra` to suffer more than on a Quest 3. That is why the default is `stock`; step 9 records what happens.
- Step 9 on a Quest 2 is the flip condition: the default becomes `high` (one line) only when `high` passes every threshold above.
- A level other than `stock` that shows a swap-chain size different from the row's size is a finding, not a typo in the sheet: write the headset and the line. One known cause on a desktop-sized start (static reading, not seen on a headset): the game skips the size query when a stored size is already set, so the key does not apply. Static reading finds one setter of that stored size, in the engine configuration, behind a flag test, passing 1920x1080; whether a Quest run sets it is not known.
- Step 5 on both headsets is the data wanted for the native-size investigation (#953).

## Report back

For each of Quest 2 and Quest 3: PASS or FAIL per step, the exact swap-chain line from steps 1, 2 and 3, the two native numbers from step 5 (and, on the Quest 2, the step 9 summary lines and `TOTAL PSS` per level), and the pulled `nevr-sentinel.log` if anything failed.
