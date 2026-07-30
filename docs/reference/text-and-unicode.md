# Text rendering and Unicode in echovr.exe

**Why this is here:** N123 made the login `displayname` come from the account's
username instead of a hardcoded literal, so a real player name can now contain
non-ASCII. This records what the game can and cannot render, measured rather than
assumed, so nobody has to re-derive it from the binary.

All addresses are `echovr.exe`. Full annotations live in ReVault; this is the
summary a runtime developer needs.

## The pipeline is genuinely UTF-8, not byte-per-glyph

| VA | Name | Role |
| --- | --- | --- |
| `0x1400DD0F0` | `DecodeUtf8Codepoint` | decodes one 1/2/3-byte sequence, returns bytes consumed |
| `0x14154E9B0` | `CFont_FindGlyph` | binary search of the sorted per-font glyph table by codepoint, with fallback |
| `0x14154EAC0` | `CFont_EmitNextGlyph` | decode → find → emit → advance cursor by decoded length |
| `0x14154EE20` | `CFont_MeasureUtf8Text` | measures the extent of a UTF-8 range |

A 3-byte sequence advances the cursor by 3 and produces exactly **one** glyph
lookup, on both the measure and the draw side. Verified by hand-tracing
`DecodeUtf8Codepoint`'s arithmetic against `E3 83 83` (U+30C3 ッ) — it yields
`0x30C3`.

`CFont_EmitNextGlyph` passes `use_fallback = 1`, so an absent codepoint renders as
the font's placeholder glyph. It is not skipped and does not corrupt the run.

## The hard limit: BMP only

`DecodeUtf8Codepoint`'s longest path is **3 bytes** and its out-parameter is a
`ushort`. Anything above U+FFFF cannot be represented at all:

- **Works:** Latin-1, Latin Extended, Greek, Cyrillic, CJK, kana, Hangul — the
  entire Basic Multilingual Plane.
- **Cannot work:** emoji and everything else astral. Not a font problem, a decoder
  problem — there is no code path that produces those codepoints.

`CFont_FindGlyph` takes a **uint32** codepoint, so the glyph table could hold
astral entries the decoder can never generate. The decoder is the pinch point.

Invalid UTF-8 returns 1 and writes the raw byte — indistinguishable from ASCII, no
error signalled.

## Fonts: CJK ships, but selection is per-language

`content/engine/core/fonts/` contains 14 Latin-only fonts **plus** exactly the
three Windows-standard CJK UI fonts — `malgun.ttf` (Korean), `MEIRYO.TTC`
(Japanese), `msyh.ttc` (Simplified Chinese). All three contain U+30C3 and broad
kana/Hangul/ideograph coverage. The Latin fonts all carry U+00E9 (é), confirming
they are genuine Latin-range fonts rather than truncated files.

The atlas-miss diagnostic at `0x141DC1A80` is
`"[FONT] Missing character with the ID %u being used from the atlas %s in the
language %s"` — **parameterised by language**, which alongside the loader pattern
`"%s|engine|core|fonts|%s-%s.ttf"` and literal `"Japanese"`/`"Korean"` strings is
strong evidence that atlases are per-language rather than global.

**Not confirmed:** the function that reads the active UI language and picks a font
file was never located, so per-language selection is inferred from co-occurrence,
not from a traced code path. **Also not determined:** whether a runtime atlas is a
curated subset baked from a character list or effectively the full font cmap — a
shipped `.ttf` containing a glyph does not prove that glyph is in the atlas a
given client builds.

## What this means in practice

| Question | Answer |
| --- | --- |
| Can a display name contain katakana? | The client can decode and render it **if** the viewer's active font covers it. |
| Will an English-UI player see it? | **Unknown** — depends on the unresolved per-language selection above. Cheapest resolution is empirical: set a CJK name and look at it from an English client. |
| Emoji? | **No**, at any font. The decoder cannot produce astral codepoints. |
| Is the server the blocker today? | Historically yes — Nakama's `sanitizeDisplayName()` transliterates via anyascii, filters to an allowlist regex, then requires at least one `[A-Za-z]` or discards the name. A name that is only non-Latin comes out empty. |

So the *client* is more capable than the *service* permits. Any decision to allow
non-ASCII names is a service-side policy change first, and only then a question
about fonts.
