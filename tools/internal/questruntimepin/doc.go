// Package questruntimepin pins the reference runtime's libr15 checksum list to the libr15 the APK carries (#1737).
//
// WHY. The reference runtime (lib/arm64-v8a/libvrapi.so of the reference APK) holds twelve modules that act only on a
// libr15 whose md5 is on the list each module carries (custctx, memsys, voipdrop and others). The libr15 this project's
// APK carries is the reference file after the host patcher (quest_patch_libr15) and so has an md5 none of the lists
// names; every one of those modules logs "OFF: libr15.so md5 ... is none of ..." and changes nothing. The md5 of the
// patched file is a function of every input byte and of the patch table, so it is never a constant: the pin is computed
// from the staged file on every build.
//
// WHAT THE LIST IS IN THE BINARY. The compare is not a string compare against a text in .rodata. Each module's
// strcmp(hex, entry) was compiled to four 64-bit XORs against constants built by movz + three movk instructions
// (sixteen instruction words per module for one entry), so one entry of the list is written in two forms: twelve ASCII
// copies (they feed the %.8s of each module's OFF line) and twelve sites of sixteen immediate words, one site per
// module. Replacing the ASCII copies alone changes nothing the gate reads. PinData replaces both forms of one entry,
// the entry at replaceIndex of the table (the first list of consecutive 32-hex texts in the file; index 0 holds
// expectOldMD5, default StockEntryMD5, the md5 the reference runtime's list opens with and no build of this project
// carries). The position alone never picks the entry: PinData refuses, naming the md5 it expected and the one it found
// (a WrongEntryError), unless the entry at the index is that md5 (or the md5 being pinned, a file already pinned) and
// that md5 has its twelve ASCII copies and its twelve immediate sites. The other entries stay, so a libr15 that is the
// reference's own is still accepted.
//
// PinData finds the twelve ASCII copies and the twelve sites (48 runs, 192 words) of the entry, asserts those exact
// counts, rewrites both forms in place (the size does not change and only the imm16 field of each word and the 32 text
// bytes of each copy change), then re-reads the result and checks it: the new text has twelve copies and twelve sites
// at that index, the old text none, and no byte outside the copies and the imm16 fields differs. It is idempotent: the
// entry already holding the md5 is a no-op that returns the input unchanged. It refuses (a PinError) any other count,
// an md5 that is already present anywhere else in the file, and a file that is not an AArch64 ELF. CarriesTable says
// whether a file carries the table at all (the store's own library carries none, and the recipe leaves it alone).
//
// The command is tools/cmd/quest-runtime-pin; fixture builds the synthetic runtimes the tests use.
package questruntimepin
