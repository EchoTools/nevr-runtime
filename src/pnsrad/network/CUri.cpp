#include "src/pnsrad/network/CUri.h"
#include <cstring>

namespace NRadEngine {

// @0x18022b940 — scheme name table
extern const char** g_uri_scheme_names;

// @0x18009fc40 — CUri::ToString
// @confidence: H
void* CUri_ToString(CUri* uri, void* out_mem_block) {
    int64_t total_len = 0;
    int64_t scheme_len = 1;  // at least 1 for null terminator

    // Calculate scheme length
    if (uri->scheme_index != 0) {
        extern int64_t strlen_wrapper(const char* str);
        scheme_len = strlen_wrapper(g_uri_scheme_names[uri->scheme_index]);
        scheme_len += 2;  // for ":"
    }

    // Calculate authority length
    if (uri->has_authority != 0) {
        int64_t auth_len = scheme_len + 2;  // for "//"

        // Userinfo length (if present and > 1)
        int64_t ui_len = uri->userinfo_len - 1;
        if (uri->userinfo_len == 0) ui_len = 0;
        if (ui_len != 0) {
            ui_len = uri->userinfo_len - 1;
            if (uri->userinfo_len == 0) ui_len = 0;
            auth_len += ui_len + 1;  // +1 for '@'
        }

        // Hostname
        int64_t host_len = uri->hostname_len - 1;
        if (uri->hostname_len == 0) host_len = 0;
        scheme_len = auth_len + host_len;

        // Port
        if (uri->port != 0) {
            scheme_len += 6;  // ":NNNNN" max
        }
    }

    // Path
    int64_t path_len = uri->path_len - 1;
    if (uri->path_len == 0) path_len = 0;
    total_len = scheme_len + path_len;

    // Query
    int64_t query_len = uri->query_len - 1;
    if (uri->query_len == 0) query_len = 0;
    if (query_len != 0) {
        query_len = uri->query_len - 1;
        if (uri->query_len == 0) query_len = 0;
        total_len += query_len + 1;  // +1 for '?'
    }

    // Fragment
    int64_t frag_len = uri->fragment_len - 1;
    if (uri->fragment_len == 0) frag_len = 0;
    if (frag_len != 0) {
        frag_len = uri->fragment_len - 1;
        if (uri->fragment_len == 0) frag_len = 0;
        total_len += frag_len + 1;  // +1 for '#'
    }

    // Initialize output buffer
    auto* alloc = get_tls_context();
    init_buffer_context(out_mem_block, total_len, 0, alloc);
    *reinterpret_cast<uint64_t*>(reinterpret_cast<uintptr_t>(out_mem_block) + 0x20) = 0;
    *reinterpret_cast<int64_t*>(reinterpret_cast<uintptr_t>(out_mem_block) + 0x28) = total_len;
    *reinterpret_cast<uint64_t*>(reinterpret_cast<uintptr_t>(out_mem_block) + 0x30) = 0;

    // Build URI string

    // Scheme
    if (uri->scheme_index != 0) {
        mem_block_append_string(out_mem_block, g_uri_scheme_names[uri->scheme_index], -1);
        mem_block_append_char(out_mem_block, ':');
    }

    // Authority
    if (uri->has_authority != 0) {
        extern const char* g_authority_prefix;  // @0x18022b868 — "//"
        mem_block_append_string(out_mem_block, &g_authority_prefix, -1);

        // Userinfo@
        if (uri->userinfo_len != 0 && uri->userinfo_len != 1) {
            mem_block_append_block(out_mem_block, uri->userinfo);
            mem_block_append_char(out_mem_block, '@');
        }

        // Hostname
        mem_block_append_block(out_mem_block, uri->hostname);

        // Port
        if (uri->port != 0) {
            char port_buf[512];
            char port_str[512];
            auto* fmt_result = format_string(port_buf, ":%hu", static_cast<uint32_t>(uri->port));
            // Copy formatted string
            auto* src = reinterpret_cast<uint64_t*>(fmt_result);
            auto* dst = reinterpret_cast<uint64_t*>(port_str);
            for (int i = 0; i < 64; i++) dst[i] = src[i];

            int64_t port_len_minus_2 = 4 - 2;  // approximate
            mem_block_append_string(out_mem_block, port_str, port_len_minus_2);
        }
    }

    // Path
    mem_block_append_block(out_mem_block, uri->path);

    // Query
    if (uri->query_len != 0 && uri->query_len != 1) {
        mem_block_append_char(out_mem_block, '?');
        mem_block_append_block(out_mem_block, uri->query);
    }

    // Fragment
    if (uri->fragment_len != 0 && uri->fragment_len != 1) {
        mem_block_append_char(out_mem_block, '#');
        mem_block_append_block(out_mem_block, uri->fragment);
    }

    return out_mem_block;
}

} // namespace NRadEngine
