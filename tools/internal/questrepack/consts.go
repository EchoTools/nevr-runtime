package questrepack

import "time"

// The APK layout the repack writes and checks (ADR 0021, 0019, 0029, 0040, 0046; the Python tool these came from).
const (
	abiDir            = "lib/arm64-v8a"
	manifestName      = "libnevr_plugin_manifest.so"
	manifestEntry     = abiDir + "/" + manifestName
	manifestSchema    = 1
	variantStamp      = "libnevr_build_variant.so"
	variantEntry      = abiDir + "/" + variantStamp
	origLoaderName    = "libovrplatformloader_orig.so"
	shimName          = "libovrplatformloader.so"
	replacedOnPurpose = abiDir + "/" + shimName
	patchedLibrary    = abiDir + "/libr15.so"
	pinnedLibrary     = abiDir + "/libvrapi.so"
	libr15Path        = abiDir + "/libr15.so"
)

// variantWords are the variant words: `q15t` and `combat` turn the combat area on (docs/adr/0046 rule 5), `perf-probe`
// is the measurement build (ADR 0037), `arena` is the release's APK built from the store APK with no combat (#2298).
var variantWords = []string{"q15t", "combat", "perf-probe", "arena"}

// stagedByRecipe are the libraries the recipe stages itself: the shim under the loader's name, the store's loader
// renamed, the manifest and the stamp.
var stagedByRecipe = []string{shimName, origLoaderName, manifestName, variantStamp}

// variantMarker is the event name only a variant build compiles in (NEVR_LIBR15_EXTRA_ROWS): a NUL-bounded string.
const variantMarker = "\x00libr15_extra_rows\x00"

// Byte and bit values of ELF, zip and the stamp.
const (
	ptNote            = 4
	ntGNUBuildID      = 3
	nevrNoteType      = 1
	emAArch64         = 183
	elfClass64        = 2
	elfDataLE         = 1
	elfHeaderBytes    = 64
	elfPhdrMinBytes   = 56
	elfPhoffOffset    = 0x20
	elfPhentsizeOff   = 0x36
	elfPhnumOff       = 0x38
	elfMachineOffset  = 18
	elfMinHeaderLen   = 20
	noteHeaderBytes   = 12
	noteAlign         = 4
	zipCreatorUnix    = 3
	zipEncryptedBit   = 0x1
	zipModeShift      = 16
	unixModeTypeMask  = 0o170000
	unixModeRegular   = 0o100000
	unixModeDirectory = 0o040000
	libPrefixLen      = 3
	soSuffixLen       = 3
	probTail          = 500
)

// The patcher reads and writes one file of about 58 MB; a run that takes longer than this is a hang.
const patcherTimeoutDefault = 600 * time.Second

// The bundled content pack (docs/adr/0029-quest-bundled-content-pack.md).
const (
	packDir          = "assets/nevr-pack"
	packJSONEntry    = packDir + "/pack.json"
	packFilesDir     = packDir + "/files"
	packSchema       = 1
	maxPackFiles     = 8
	maxPackFileBytes = (1 << 31) - 1
	apkLimitBytes    = 0xFFFFFFFF
	apkSlackBytes    = 8 * 1024 * 1024
	maxEntryNameLen  = 512
	maxEntryParts    = 16
	maxComponentLen  = 255
	maxRequiresSums  = 2
	ascii7DEL        = 0x7F
	asciiControlMax  = 0x20
)

// The script libraries (docs/plans/2026-10-31-combat-beta-local.md, M5; #1737): the nine stock libraries a combat build
// redirects, given to the APK as lib<hash16>_nevr.so. src/quest/combat/script_redirect_list.h holds the same list.
var combatRedirectHashes = []string{
	"37f88adb0d8066f7", "3c11128c3bd783dd", "4b65d1b009973281", "6a0f689e5b91c666", "80461e04c6d82c21",
	"812075466da9e147", "954c89b6c61625f6", "a5c64faa725f2334", "ac4cd8e0a786d5c5",
}

const (
	redirectSuffix  = "_nevr"
	libsSums        = "SHA256SUMS"
	libsReplace     = "REPLACE"
	stagedReplace   = "lib-replace.txt"
	stagedScript    = "script-libs.txt"
	redirectedCount = 9
)

// The data step's pin (src/quest/data/plan.h) and the stock manifest name: read by the pack staging.
const planHeaderPath = "src/quest/data/plan.h"
