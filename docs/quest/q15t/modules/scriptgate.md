# Module scriptgate: the combat locks of the menu scripts open, every other script keeps the headset's real kind

Issue #1601 (the menu gate). Option `Q15T_GATE_SCRIPTS`, default ON. Kill file `scriptgate.off` in the plugin data directory.
Source: `src/quest/plugins/q15t/src/scriptgate.{h,cpp}`. Replaces `Q15T_GATE_LINK` (now OFF by default), which answers one kind to
every script.

## 1. What the game does

- `CVR::HeadsetType()` returns 3 (a Quest) on this build (libr15 ReVault 0x212ba74, `BindExpression<CR15HeadsetTypeExpression>`
  switch: 0/1 rift, 2 rifts, 3 quest, 4 link, writing one byte of the expression's output block).
- Each script library carries its own `R15HEADSETTYPEEXPRESSION::Bind` (ReVault `lib555db38933fef859.so` 0x122e68, 352 bytes). It
  registers the four outputs with the engine by name (`"rift"`, `"rifts"`, `"quest"`, `"link"`, one call each), passing the address of
  the library's own word `output_<name>_offset` and of its `output_<name>_func`. The engine's lookup
  (`LookupExpDataBinding<...SOutput>`, libr15 0x212bb2c) stores the output's slot in that word: rift 0, rifts 1, quest 2, link 3.
- The library's condition code reads the word each time it evaluates (`lib555db38933fef859.so` 0x4b160..0x4b164: a load of the
  word, then the call that reads the flag at `object + 0xa + offset`). The four words are exported data symbols of every such
  library (`readelf -s`: `_ZN24R15HEADSETTYPEEXPRESSION19output_quest_offsetE`, 4 bytes, `.bss`, writable, outside RELRO).

## 2. What the module does

For each library in `kScriptGateLibs` the tick finds the library when the engine has loaded it (`dlopen(name, RTLD_NOLOAD)`, a
lookup every second until all are found; the reference is kept), resolves its two words by `dlsym`, and, where the quest word holds 2
and the rifts word holds 1, stores 1 in the quest word. The library's "quest" output then reads the rifts flag, which a Quest never
sets, so the scripts of that library take the PC branch. A quest word of 0 is a library the engine has not bound yet (looked at again
every frame); 1 is done; any other value, or a rifts word that is not 1, refuses that one library with an ERROR line naming it and
the values, and writes nothing. A write to a library whose word returns to 2 (the engine bound it again) is made again; the first
four writes on a library are logged.

The set is the libraries whose headset-type uses are all combat locks: the combat buttons and sub-buttons, FIND COMBAT, the combat
mode card, the combat level pages and text, the "combat allowed" prefab. That is the scope the upstream combat port patches. It is
taken from that port's list, not re-derived here: that every use in these libraries is a combat lock is the port's statement, and
that the same expression is used by other libraries it leaves alone is shown by ReVault (the headset-type node class appears in
many script libraries of the project). Every other script keeps the real kind (a Quest), which the main-menu PLAY path needs: an
answer of "not a Quest" given to every script sent PLAY down a branch with no target and crashed it (`CSendEventNode::Enter`).

No code word and no page protection call: one aligned 32-bit store into a data page that is already writable.

## 3. Prediction (before any run)

- Log, once per library as the engine binds it (first frames to the first menu): `scriptgate: quest output of this script library now
  reads the rifts flag ...` with `lib=lib<hash>.so quest_offset_from=2 quest_offset_to=1`, up to 20 lines; then
  `scriptgate: every script library of the set is settled` with `libraries=20 bound=20 refused=0 writes=20`.
- In the menu: the combat entries (FIND COMBAT, the combat mode card, the combat level pages) are not locked or hidden for the
  Quest; main-menu PLAY does not crash.
- Falsifiers: `refused>0` (a library's words are not as read here: the line names it); `bound` stays under 20 after the menu has
  been shown (a library never found: the engine loads it under another name or after this point); combat entries still locked with
  `bound=20` (the locks are read through another path than the library's condition); PLAY crashing with `scriptgate` bound=20 (the
  crash is not the headset kind).
- The lobby's combat wing is not covered: the upstream port opens it by changing two node functions of the lobby script library
  (`libf808c072bb49da47.so`), which is code in the library and not a binding word, so it is outside this module.

## 4. Not shown

Nothing here has run on a headset. The library set is not independently checked. The first frame a library is bound, a condition
may evaluate once before the tick writes (the tick runs every frame, on the game thread).
