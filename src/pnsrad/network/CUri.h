#ifndef PNSRAD_NETWORK_CURI_H
#define PNSRAD_NETWORK_CURI_H

/* @module: pnsrad.dll */
/* @purpose: CUri — URI string builder (scheme://[user@]host[:port]/path[?query][#fragment]) */

#include <cstdint>
#include <cstddef>

namespace NRadEngine {

// Forward declarations
extern void  init_buffer_context(void* ctx, uint64_t initial_size, uint64_t flags, void* allocator);
extern void* get_tls_context();
extern void* format_string(void* out_buf, const char* fmt, ...);
extern void  mem_block_append_string(void* block, const void* str, int64_t max_len);  // @0x18008ba20
extern void  mem_block_append_char(void* block, char ch);  // @0x18008b970
extern void  mem_block_append_block(void* dst, void* src);  // @0x18008b8c0

// ============================================================================
// CUri — parsed/assembled URI structure
// ============================================================================
// Layout (0x150 bytes):
//   +0x00: uint16_t scheme_index    — index into PTR_s_undefined_18022b940 scheme table
//   +0x02: uint16_t port            — port number (0 = not specified)
//   +0x04: int32_t  has_authority   — non-zero if authority component present
//   +0x08: CMemBlock userinfo       — user@host portion before '@' (0x30 bytes)
//   +0x38: int64_t  userinfo_len    — length of userinfo (0 = empty)
//   +0x40: CMemBlock hostname       — host portion (0x30 bytes)
//   +0x70: int64_t  hostname_len    — length
//   +0x78: CMemBlock path           — path portion (0x30 bytes)
//   +0xA8: int64_t  path_len        — length
//   +0xB0: CMemBlock query          — query string after '?' (0x30 bytes)
//   +0xE0: int64_t  query_len       — length
//   +0xE8: CMemBlock fragment       — fragment after '#' (0x30 bytes)
//   +0x118: int64_t fragment_len    — length

/* @addr: 0x18009fc40 */
/* @size: 0x150 */
/* @confidence: H */
struct CUri {
    uint16_t scheme_index;    // +0x00
    uint16_t port;            // +0x02
    int32_t  has_authority;   // +0x04
    uint8_t  userinfo[0x30];  // +0x08: CMemBlock
    int64_t  userinfo_len;    // +0x38
    uint8_t  hostname[0x30];  // +0x40: CMemBlock
    int64_t  hostname_len;    // +0x70
    uint8_t  path[0x30];      // +0x78: CMemBlock
    int64_t  path_len;        // +0xA8
    uint8_t  query[0x30];     // +0xB0: CMemBlock
    int64_t  query_len;       // +0xE0
    uint8_t  fragment[0x30];  // +0xE8: CMemBlock
    int64_t  fragment_len;    // +0x118
    uint8_t  _pad[0x38];     // +0x120: padding to 0x150
};
// MSVC packs this to 0x150; MinGW may pad to 0x158 due to alignment differences
#ifdef _MSC_VER
static_assert(sizeof(CUri) == 0x150);
#endif
static_assert(offsetof(CUri, scheme_index) == 0x00);
static_assert(offsetof(CUri, port) == 0x02);
static_assert(offsetof(CUri, has_authority) == 0x04);
static_assert(offsetof(CUri, userinfo_len) == 0x38);
static_assert(offsetof(CUri, hostname_len) == 0x70);
static_assert(offsetof(CUri, path_len) == 0xA8);
static_assert(offsetof(CUri, query_len) == 0xE0);
static_assert(offsetof(CUri, fragment_len) == 0x118);

// @0x18009fc40 — CUri::ToString
// Assembles the URI from its components into a CMemBlock output buffer.
// Format: scheme:://[userinfo@]host[:port]/path[?query][#fragment]
// @confidence: H
void* CUri_ToString(CUri* uri, void* out_mem_block);

// Scheme name table (indexed by scheme_index)
// @0x18022b940 — PTR_s_undefined_18022b940
extern const char** g_uri_scheme_names;

} // namespace NRadEngine

#endif // PNSRAD_NETWORK_CURI_H
