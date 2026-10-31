# Headset sheet: Quest verification batch

One sitting, one development build, six open checks: #1161 (no libr15 code-page write), #947 (the permission request's code), #1165 (the sign-in title),
#1171 (the main-menu page is released cleanly), #1173 (the main-menu cards) and #944 with PR #1183 (the logged-in user
id). Run it on a Quest 2 and, if one is at hand, a Quest 3. Fill each headset column with PASS or FAIL and paste the
line you saw. A column left empty means that headset has not been run.

Two checks from the same day are not in this sheet. #1166 needs the libr15 patched at build time (#1132), which no
build has yet. #1168 is a PC check, run by an agent on a throwaway game copy.

Every expected line below comes from the code of the build, not from an earlier run, except where a row says it was
seen before. A line that differs from the row is a finding: paste it. Do not try a variant. It goes back to reverse
engineering.

## Before you start

**The build.** A development APK from `main` that contains PR #1183. If #1183 has not merged, the build must be made
from its branch merged with `main`; without it, step 4 has no `quest_logged_in_user_id` line, so mark step 4 NOT RUN. The same goes for PR #1207 (#947): without it, step 1's #947 lines are absent; follow that row's "without #1207" text.

**You need:** adb over USB or Wi-Fi, a phone or computer to enter the sign-in code, and the headset on Wi-Fi.

**Headset facts**, pasted once per headset into "Report back" (the sentinel's log does not record which headset it
ran on):

```
adb shell getprop ro.product.model
adb shell getprop ro.build.display.id
adb shell getprop ro.build.version.incremental
adb shell dumpsys package com.readyatdawn.r15 | grep -E 'versionName|versionCode'
```

The game files folder is `/sdcard/Android/data/com.readyatdawn.r15/files/`.

**Capture everything.** In a second terminal, start this before step 1 and leave it running to the end. The game's
own lines are not under one known tag, and the `execmod` and crash lines come from the system, so the whole logcat
is needed:

```
adb logcat -c && adb logcat -v time > batch-<headset>.log
```

Search it with `grep -F '<text>' batch-<headset>.log`. If logcat rolled over, also pull the sentinel's own log at the
end: `adb pull /sdcard/Android/media/com.readyatdawn.r15/files/nevr-sentinel.log .` (on a headset where that file is absent, the same name in `/sdcard/Android/data/com.readyatdawn.r15/files/`)

**Sentinel lines** are JSON under the logcat tag `NEVR-Sentinel`, for example `{"event":"code_write_skipped",...}`.
A row names the event and the fields to check. Counters appear as `{"event":"hook_counter","counter":"<name>",
"value":<n>,"why":"first_change"}` when they first move. A counter that never moved is logged once, after the
reporting window, with `"why":"never_fired"`.

## Steps

Steps 1 to 6 are one launch, from cleared data. Step 7 is a second launch.

| # | What you do | What you should see | Line to find | Quest 2 | Quest 3 |
| --- | --- | --- | --- | --- | --- |
| 1 | Clear the game's data so the first sign-in shows a code. First list the three places the game data can be: `adb shell ls /sdcard/readyatdawn/_data /sdcard/Android/media/com.readyatdawn.r15/files /sdcard/Android/data/com.readyatdawn.r15/files`. `pm clear` deletes the game's folders under both `Android/data/com.readyatdawn.r15` and `Android/media/com.readyatdawn.r15`; it does not touch `/sdcard/readyatdawn`. So if `_data` is in either Android folder, back it up first (`adb pull <that folder>/_data _data-backup`) or move it to `/sdcard/readyatdawn/_data`, or skip `pm clear`. Save anything else in that files folder you want to keep the same way. Then `adb shell pm clear com.readyatdawn.r15`, push back your `config.yaml` if you had one (`adb push files-backup/config.yaml /sdcard/Android/data/com.readyatdawn.r15/files/config.yaml`), and start the game from the headset menu. `pm clear` also removes the Storage permission: grant it again when asked (the grant restarts the game, which is expected). Accept any other permission prompt the game shows. | The game starts and does not close. | **#1161:** one `sentinel_ctor` line with `"action":"done"` and `"steps_failed":0`. Write its `steps_ok` and `steps_skipped`. Exactly five `code_write_skipped` lines, each with `"reason":"execmod_denied_on_libr15"` and `"waits_on":"#1132"`, one for each `"feature"`: `lobby_full`, `menu_version`, `ui_texture`, `splash_skip` and `credits_text`. In the whole log: `grep -F execmod batch-<headset>.log` finds no line naming `libr15.so`, and neither `grep -F SIGSEGV batch-<headset>.log` nor `grep -F 'Fatal signal' batch-<headset>.log` finds a line. **#947, with PR #1207 in the build:** if the game showed the permission prompt, one `permission_request` line with `"result":"requested"`, `"path":"ours"`, `"permissions":3` and `"request_code":1`, then `permission_wait` with `"state":"waiting"` and, after you answer the prompt, `"state":"granted"`. The game's own line `Requesting 3 android permissions` is absent, and `grep -F IllegalArgumentException batch-<headset>.log` finds nothing. A `permission_request` line with `"path":"game"` is a finding: paste it. No prompt means no `permission_request` line; write that the prompt did not show. **Without #1207:** if the game closes right after the prompt, search for `IllegalArgumentException` and `requestPermissions` and paste what you find. | | |
| 2 | Wait for the sign-in screen. Do not sign in yet. | **#1165:** the screen is titled **SIGN IN**, not LOGIN FAILED, and shows a code. | `login_title_install` with `"result":"installed"`. (The issue says `result=ok`; the code writes `installed`.) Then `hook_counter` with `"counter":"login_title_replaced"` and a `value` of 1 or more. | | |
| 3 | Sign in: on the phone or computer, open the sign-in page `docs/quest/SIGN-IN.md` names, enter the code, then select RETRY on the headset when it says it is signed in. | **#1165:** the title becomes **SIGNED IN**. Then the game logs in and reaches the main menu. | The game's own line `NetGame switching state (from logging in, to logged in)`. | | |
| 4 | Nothing to do: read the log of the launch so far. | (nothing to look at) | **#944, PR #1183:** one `quest_login_prerequisites_install` line with `"status":"installed"`, `"logged_in_user_id":1` and `"social_facade":"installed"`. Exactly one `quest_logged_in_user_id` line, with `"answer":"sdk"`, `"sdk_zero":0` and `"reason":"sdk_nonzero"`. The game's line `[OVR] Logged in user app-scoped id: N`, with N not 0: write whether it is nonzero, not the number. `hook_counter` `prereq_user_id_calls` with `"value":1`, and `prereq_user_id_stand_ins` only as `"why":"never_fired"`. `quest_login_send` with `"account_member_stand_in":0`. No line with `There's a problem with the logged in user`. | | |
| 5 | On the main menu, look at the card row. Take a screenshot (`adb exec-out screencap -p > menu-<headset>.png`). Select CREATE GAME and look at what opens, then back out to the main menu. | **#1173:** the main menu shows a CREATE GAME card and a JOIN VIA CODE card. CREATE GAME does not open the combat lockout panel. | `menu_cards_install` with `"result":"installed"`. When the menu first shows: `menu_cards_page` with `"pin":"match"` and `"bytes":81160`, then `menu_cards_swap` with `"result":"swapped"`, `"bytes_before":81160` and `"bytes_after":82568`. No `menu_cards_skip` line. (Seen before on a Quest 2 dev build, under #1159.) | | |
| 6 | Leave the main menu for the social lobby, stay a minute, then go back to the main menu. Do it twice. Then count. | **#1171:** the game stays up through each leave and return. | Count the `menu_cards_page` lines and the `menu_cards_swap` lines (`grep -cF menu_cards_page`, `grep -cF menu_cards_swap`) and write both counts. If they are more than 1, the game unloaded the page and loaded it again: each later pair must look like step 5's. If they stay at 1, the game kept the page loaded, which is the answer to "is the page ever unloaded". Write which one happened. No `SIGSEGV`, `Fatal signal` or `abort` line anywhere in the log after step 5. | | |
| 7 | A launch that never reaches a code. Turn the headset's Wi-Fi off. Force-stop (`adb shell am force-stop com.readyatdawn.r15`) and start the game. Wait two minutes on the screen it shows. Then turn Wi-Fi back on. With adb over Wi-Fi, connect by USB for this step, or skip it and say so. | **#1165:** the screen stays **LOGIN FAILED** and no code is shown. | `hook_counter` `login_title_kept` with a `value` of 1 or more. `login_title_replaced` does not move in this launch: either no line for it, or only `"why":"never_fired"`. | | |

## Pass and fail

A step passes when every line its row names is in the log, with the fields shown, and what you see matches.

These count as a fail:

- **Steps 1–6:**
  - the game closes;
  - any `execmod` line for `libr15.so`;
  - a `SIGSEGV` or `Fatal signal`;
  - `steps_failed` above 0;
  - fewer or more than five `code_write_skipped` lines;
  - an inline-hook or credits line other than `code_write_skipped`.
- **Step 2:** the code screen titled LOGIN FAILED, or `login_title_replaced` at 0 while the code shows.
- **Step 4:** `"status":"partial"`, `"social_facade":"not_installed"`, `account_member_stand_in` of 1, or the "problem with the logged in user" error.

Step 4 has one result that is not a fail. A `quest_logged_in_user_id` with `"answer":"stand_in"` and `"sdk_zero":1` means the SDK returned 0 on this account, which #1183 was built for and nobody has seen with our build. Mark the step PASS if the login still went on, and say so in the cell.

**Step 6:** a page count of 1 is a result, not a fail. A crash at the return to the main menu is the fail #1171 looks for.

## Report back

For each headset:

- the four headset facts;
- PASS or FAIL per step, with the lines the rows name pasted (the five `code_write_skipped` lines, the `sentinel_ctor` line, the `quest_logged_in_user_id` line, the `menu_cards_*` lines and the counts from step 6, the `login_title_*` counters from steps 2 and 7);
- the screenshot from step 5;
- `batch-<headset>.log`, and `nevr-sentinel.log` if logcat rolled over.

Each issue closes on its own lines: #1161 (step 1), #947 (step 1, with #1207 merged), #1165 (steps 2, 3 and 7), #1173 (step 5), #1171 (step 6), #944 (step 4, with #1183 merged).
