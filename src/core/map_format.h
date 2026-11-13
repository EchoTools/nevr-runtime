/* The numbers and names the custom-map channel's formats define, each cited to its format, so no other map file carries a bare
 * literal for one (docs/adr/0047-custom-maps-channel.md):
 *   - minisign's public key and signature (the minisign tool's own file format, minisign 0.12): what minisign_verify.cpp reads;
 *   - the signed channel manifest, `nevr-maps-manifest` v1, and the signed object that carries it (ADR 0047 decision 1);
 *   - a map's package index, `nevr-map-package` v1 (ADR 0047 decisions 1, 2, 5 and 9);
 *   - the recipe format a stock-derived asset ships as (ADR 0047), whose numbers the publisher shares through
 *     tests/vectors/maps/constants.json;
 *   - the cache index, `nevr-maps-index` v1 (ADR 0047 decision 4).
 * A number that limits how much memory, disk or descriptors this process uses is not here: it is a share of what the device reports
 * (ADR 0045 decision 8; src/core/cargo_limits.h, src/core/device_memory.h). Hexadecimal and SHA-256 text are src/core/cargo_format.h's.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace nevr_maps {

// ---- minisign (the tool's key and signature files). A public key line is base64 of: the algorithm "Ed", an 8-byte key id, a
// 32-byte Ed25519 key. A signature line is base64 of: the algorithm ("ED" prehashed, "Ed" legacy), the key id, a 64-byte Ed25519
// signature. The global signature is 64 bytes over the signature and the trusted comment. "ED" signs the BLAKE2b-512 of the message.
inline constexpr char kMinisignAlgorithmFirst = 'E';
inline constexpr char kMinisignKeyAlgorithm = 'd';        // "Ed": the key's algorithm, and the legacy signature's
inline constexpr char kMinisignPrehashedAlgorithm = 'D';  // "ED": the only signature algorithm accepted
inline constexpr std::size_t kMinisignAlgorithmBytes = 2;
inline constexpr std::size_t kMinisignKeyIdBytes = 8;
inline constexpr std::size_t kEd25519PublicKeyBytes = 32;
inline constexpr std::size_t kEd25519SignatureBytes = 64;
inline constexpr std::size_t kBlake2b512Bytes = 64;
inline constexpr std::size_t kMinisignPublicKeyBlobBytes = kMinisignAlgorithmBytes + kMinisignKeyIdBytes + kEd25519PublicKeyBytes;
inline constexpr std::size_t kMinisignSignatureBlobBytes = kMinisignAlgorithmBytes + kMinisignKeyIdBytes + kEd25519SignatureBytes;
inline constexpr std::size_t kMinisignKeyIdAt = kMinisignAlgorithmBytes;
inline constexpr std::size_t kMinisignPayloadAt = kMinisignKeyIdAt + kMinisignKeyIdBytes;  // the key or the signature
// The signature file's four lines: untrusted comment, signature, trusted comment, global signature.
inline constexpr std::size_t kMinisignSignatureLines = 4;
inline constexpr std::size_t kMinisignSignatureLine = 1;
inline constexpr std::size_t kMinisignTrustedLine = 2;
inline constexpr std::size_t kMinisignGlobalLine = 3;
inline constexpr std::string_view kMinisignUntrustedPrefix = "untrusted comment: ";
inline constexpr std::string_view kMinisignTrustedPrefix = "trusted comment: ";
inline constexpr std::string_view kMinisignUntrustedKeyword = "untrusted comment:";  // a .pub file's first line, any spacing after
// Base64 (RFC 4648 section 4): standard alphabet, padded, four characters for three bytes, six bits a character.
inline constexpr std::size_t kBase64GroupChars = 4;
inline constexpr std::size_t kBase64GroupBytes = 3;
inline constexpr unsigned kBase64CharBits = 6;
inline constexpr int kBase64LowerFirst = 26;  // 'A'-'Z' are 0-25, 'a'-'z' 26-51, '0'-'9' 52-61
inline constexpr int kBase64DigitFirst = 52;
inline constexpr unsigned kBase64Plus = 62;
inline constexpr unsigned kBase64Slash = 63;
inline constexpr std::size_t kBase64MaxPadding = 2;
inline constexpr std::size_t kBase64TwoPadLastChar = 1;    // with "==" the group's last data character is its second ...
inline constexpr unsigned kBase64TwoPadSpareMask = 0x0F;     // ... and its low four bits must be zero
inline constexpr std::size_t kBase64OnePadLastChar = 2;    // with "=" it is the third ...
inline constexpr unsigned kBase64OnePadSpareMask = 0x03;     // ... and its low two bits must be zero

// ---- The signed object (<base_url>/guilds/<group_id>/manifest): the minisig text, a "--" line, then the manifest body.
inline constexpr std::string_view kSignedSeparator = "\n--\n";
inline constexpr std::size_t kMaxMinisigBytes = 4096;  // the signature part: four short lines, so a longer one is not a signature

// ---- nevr-maps-manifest v1.
inline constexpr const char* kManifestSchema = "nevr-maps-manifest";
inline constexpr std::uint64_t kManifestSchemaVersion = 1;
inline constexpr int kMaxJsonDepth = 16;  // both documents: deeper nesting is refused, so no two readers disagree on one
// A channel name: [a-z0-9][a-z0-9.-]{0,63}.
inline constexpr std::size_t kMaxChannelChars = 64;
// The guild a manifest is for: the lobby group id as the game service writes it, held as an opaque string. It names a URL path
// segment and a cache directory, so the only rule is that it is safe there: 1 to kGroupIdMaxBytes characters, each a lowercase
// letter, a digit or '-'. It is never parsed as a UUID, because the service may change the form of its group ids. The bound
// leaves room for both forms an id has had or may take: a canonical UUID is 36 characters and a 64-bit snowflake at most 20
// digits; 64 holds either with room to spare and stays far below a path component's limit (255 bytes on NTFS and ext4).
inline constexpr std::size_t kGroupIdMaxBytes = 64;
// A map id: [a-z][a-z0-9_]{0,54}. The Quest level name is the id and kQuestLevelSuffix, and a level name is at most
// kMaxSymbolNameChars, so an id leaves room for the suffix.
inline constexpr const char* kQuestLevelSuffix = "_lowspec";
inline constexpr std::size_t kMaxSymbolNameChars = 63;
inline constexpr std::size_t kMaxMapIdChars = kMaxSymbolNameChars - std::string_view(kQuestLevelSuffix).size();
// A version: 1 to kMaxVersionParts dotted parts of 1 to kMaxVersionDigits digits, no leading zero; a missing part is 0.
inline constexpr std::size_t kMaxVersionParts = 4;
inline constexpr std::size_t kMaxVersionDigits = 9;
inline constexpr unsigned kDecimalBase = 10;
// UTC time: YYYY-MM-DDTHH:MM:SSZ, fixed width and zero padded, so text order is time order.
inline constexpr std::size_t kUtcChars = 20;
inline constexpr std::size_t kUtcYearAt = 0, kUtcYearDigits = 4;
inline constexpr std::size_t kUtcMonthAt = 5, kUtcDayAt = 8, kUtcHourAt = 11, kUtcMinuteAt = 14, kUtcSecondAt = 17;
inline constexpr std::size_t kUtcFieldDigits = 2;
inline constexpr std::size_t kUtcDateDashes[] = {4, 7};
inline constexpr std::size_t kUtcTAt = 10, kUtcZAt = 19;
inline constexpr std::size_t kUtcTimeColons[] = {13, 16};
inline constexpr int kMonthsPerYear = 12, kMaxHour = 23, kMaxMinute = 59, kMaxSecond = 59;
inline constexpr int kDaysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
inline constexpr int kFebruary = 2;
inline constexpr int kLeapEvery = 4, kLeapCenturySkip = 100, kLeapCenturyKeep = 400;
// The largest package index a manifest may pin, and the largest manifest body: limits of the v1 documents, the same on every device,
// so every device accepts the same documents. Reading one also needs the device's memory share (MapDocumentBytes, map_manifest.h).
inline constexpr std::uint64_t kMaxManifestBytes = std::uint64_t{4} << 20;
inline constexpr std::uint64_t kMaxIndexBytes = std::uint64_t{8} << 20;
// The most config aliases one map may carry (ADR 0047 decision 5).
inline constexpr std::size_t kMaxConfigAliases = 4;

// ---- nevr-map-package v1.
inline constexpr const char* kPackageSchema = "nevr-map-package";
inline constexpr std::uint64_t kPackageSchemaVersion = 1;
// The largest piece a member may list (tests/vectors/maps/package_index_cases.json "piece_over_128mib"; the publisher cuts a piece
// before 8 MiB, ADR 0047 decision 2).
inline constexpr std::uint64_t kMaxPieceBytes = std::uint64_t{128} << 20;
// A member's path, by role: set/<platform>.nevrres, scripts/<hash16>.dll (PC) or scripts/lib<hash16>.so (Quest),
// recipe/<platform>.bin. <hash16> is the script's symbol in 16 lowercase hex digits.
inline constexpr const char* kSetPathPrefix = "set/";
inline constexpr const char* kSetPathSuffix = ".nevrres";
inline constexpr const char* kScriptPathPrefix = "scripts/";
inline constexpr const char* kPcScriptPathSuffix = ".dll";
inline constexpr const char* kQuestScriptPathPrefix = "lib";
inline constexpr const char* kQuestScriptPathSuffix = ".so";
inline constexpr const char* kRecipePathPrefix = "recipe/";
inline constexpr const char* kRecipePathSuffix = ".bin";
// A rekeyed script's edit is the immediate of a 10-byte `mov r64, imm64` (REX.W B8+r, then the 8-byte immediate): an edit at `at`
// writes 8 bytes and its instruction starts kRekeyOpcodeBytes before it.
inline constexpr std::uint64_t kRekeyEditWidth = 8;
inline constexpr std::uint64_t kRekeyOpcodeBytes = 2;
// A field edit writes 1, 2, 4 or 8 little-endian bytes.
inline constexpr std::uint64_t kFieldWidths[] = {1, 2, 4, 8};

// ---- The recipe format (ADR 0047): a stock-derived asset ships as edits against the player's own stock bytes. The values are the
// publisher's too; tests/vectors/maps/constants.json holds the same ones and map_manifest_test compares them. Each is a rule of the
// format both sides enforce, not a bound on this process's memory: the rebuild that allocates a result also checks the device's
// memory share (ADR 0045 decision 8) before it does.
inline constexpr std::size_t kStockBytesMinSize = 64;      // a stock asset or a result smaller than this is refused (recipe_tiny)
inline constexpr std::size_t kStockBlockSize = 4096;       // the stock fingerprint's block
inline constexpr std::size_t kRecipeOpsMax = 65536;        // edits in one recipe (recipe_ops)
inline constexpr std::uint64_t kRecipeInsertMax = std::uint64_t{4} << 20;      // authored bytes one recipe carries (recipe_cap)
inline constexpr std::uint64_t kRecipeMapInsertMax = std::uint64_t{64} << 20;  // authored bytes one map carries (recipe_cap)
inline constexpr std::uint64_t kRecipeResultMax = std::uint64_t{128} << 20;    // one result's size (recipe_cap)
inline constexpr std::uint64_t kRecipeInsertDivisor = 4;   // a recipe's authored bytes are at most result_size / this (recipe_cap)
inline constexpr int kRecipeFormatVersion = 2;             // 2: field edits dropped `from`

// ---- The cache index, <MapsCacheDir>/index.json, and the stored manifest beside it.
inline constexpr const char* kCacheIndexSchema = "nevr-maps-index";
inline constexpr std::uint64_t kCacheIndexSchemaVersion = 1;
inline constexpr const char* kCacheIndexFile = "index.json";
inline constexpr const char* kStoredManifestFile = "manifest.signed";
inline constexpr std::size_t kVersionDirChars = 12;  // a version directory is named for the first 12 hex digits of its index sha256
// What a recipe rebuild writes inside a version directory: the merged set as derived/<platform>.nevrres and each rebuilt script as
// derived/scripts/<hash16>.dll. They are written under derived.tmp and the directory is renamed when every output verified.
inline constexpr const char* kDerivedDir = "derived";
inline constexpr const char* kDerivedSetSuffix = ".nevrres";
inline constexpr const char* kDerivedScriptsDir = "scripts";
inline constexpr const char* kDerivedScriptSuffix = ".dll";
// A directory still being written: a version directory or the rebuild's outputs. Its name is the final one and this suffix.
inline constexpr const char* kStagingSuffix = ".tmp";

}  // namespace nevr_maps
