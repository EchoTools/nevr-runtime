# libcurl advisories that name the pinned curl, and which callers reach them

The vcpkg curl is 8.18.0 on both targets (`LIBCURL_VERSION` in `curl/curlver.h` of the `x64-mingw-static` and
`arm64-android` vcpkg trees). The dependency is held at that version; this page says which published advisories
list 8.18.0 as affected and which of them a caller in this tree can reach. A new advisory, or a caller that sets
an option named below, reopens the assessment.

Source: the advisory list in curl's `vuln.json` (`affected[].versions` containing `8.18.0`): 58 entries, all with a
fix newer than 8.18.0. Severity is the list's own: 1 High (CVE-2026-92392), 12 Medium, the rest Low.

## What each build contains

| | PC (`x64-mingw-static`) | Quest (`arm64-android`) |
| --- | --- | --- |
| vcpkg features | `core;non-http;ssl;sspi` | `core;openssl;ssl;websockets` |
| TLS backend | Schannel | OpenSSL |
| HTTP/2, HTTP/3, PSL, LDAP, SSH | not linked (no `nghttp2`, `ngtcp2`, `nghttp3`, `libpsl`, `libssh` symbol in `libcurl.a`) | not linked (same check) |
| FTP code | linked (`Curl_ftp_parselist` in `libcurl.a`) | not linked |
| WebSocket API (`curl_ws_*`) | linked | linked |

Checked with `nm` on each `libcurl.a`; the feature lists are `share/curl/vcpkg_abi_info.txt` of each tree.

## Callers and the options they set

| Caller | Request | Redirects | Proxy | Credentials, cookies, netrc | Protocols |
| --- | --- | --- | --- | --- | --- |
| Quest `src/quest/data/curl_transport.cpp` | HTTPS GET, `Range` | off | forced off | none | `https` |
| Quest `src/quest/auth/curl_http.cpp` | POST with body | off | forced off | `Authorization` as a header only | `https` (`http,https` when plain HTTP is allowed) |
| Quest `src/quest/net/curl_ws_connector.cpp` | WebSocket (`CONNECT_ONLY`) | off | `NOPROXY "*"` | none | `wss` |
| PC `src/runtime/patch/asset_cdn.cpp` | HTTPS GET (manifest, assets) | **on** (`FOLLOWLOCATION 1`) | environment | none | libcurl default |
| PC `src/runtime/patch/invite_transport_curl.cpp` | POST with body, `Authorization: Bearer` header | off | environment | header only | libcurl default |
| PC `src/core/auth_token_refresh.h`, `src/modules/token-auth/src/token_auth.cpp`, `src/runtime/server/gameserver_serverdb.cpp` | POST with body | off (default) | environment | none | libcurl default |

Every caller uses one easy handle per request and no multi, share, MIME, cookie, netrc, `USERPWD`, `HTTPAUTH`,
`REFERER`, HSTS or `PINNEDPUBLICKEY` option (`git grep -nE "CURLOPT_(COOKIE|NETRC|USERPWD|HTTPAUTH|REFERER|HSTS|PINNEDPUBLICKEY)|curl_(multi|share|mime)" -- src`
finds none outside tests).

## Advisories that apply

| Advisory | Severity | Caller | Why it applies |
| --- | --- | --- | --- |
| CVE-2026-11586 (WebSocket Auto-PONG memory exhaustion) | Low | Quest `curl_ws_connector.cpp` | The connector reads with `curl_ws_recv` and leaves `CURLOPT_WS_OPTIONS` unset, so libcurl answers each PING itself and keeps the unanswered frames without a bound (its comment: "libcurl answers pings itself"). A server that floods PINGs can exhaust the process's memory. The peer is the configured service over verified TLS, so this needs a hostile or compromised service. Fixed in 8.20.1 and 8.21.0. |
| CVE-2026-6253 (credentials of one proxy passed to a second proxy on redirect) | Medium | PC `asset_cdn.cpp` | The only caller that both follows redirects and honours the environment's proxies. It needs per-scheme proxies (`http_proxy` and `https_proxy`) that differ, the first with credentials, and a redirect that changes scheme. The code sets no proxy and no credential; the exposure is the player's own proxy credentials. |

## Advisories that do not apply, by reason

- **Upload over HTTP/2** (CVE-2026-92392, the High one), **HTTP/2 server push and stream dependency** (CVE-2026-18924, 10536), **HTTP/3 and QUIC** (CVE-2026-11352, 9545): the HTTP/2 and HTTP/3 libraries are not linked in either build.
- **Cookies, netrc, referer, credentials, Digest and Negotiate authentication, proxy credentials** (CVE-2026-92386, 86412, 82209, 80255, 8924, 6276, 94266, 86414, 9079, 8927, 8926, 8458, 7168, 6429, 3784, 3783, 19931, 11856, 5545, 1965, 9546): no caller sets the input the advisory needs; a bearer token goes in a header, not in `CURLOPT_USERPWD` or `CURLOPT_XOAUTH2_BEARER`.
- **Connection reuse** (CVE-2026-107929, 80231, 8932, 8286, 4873, 11564, 5773, 3805): one easy handle per request, destroyed afterwards, so nothing is reused.
- **Multi handle, pause or callbacks** (CVE-2026-80229, 9080, 92387): no multi handle, no `curl_easy_pause`, no socket callback.
- **Pinning** (CVE-2026-80230, 87866): `PINNEDPUBLICKEY` is not set.
- **TLS backends this tree does not link** (CVE-2026-82208, 87866 wolfSSL; 107928 GnuTLS; 7009 Apple SecTrust): PC links Schannel and Quest OpenSSL.
- **Protocols no caller requests, and the `curl` tool** (CVE-2026-92388, 92389, 92391, 92390, 87865, 87864, 87863, 87862, 86410, 86409, 13608, 12064, 9547, 8925): FTP, TFTP, IMAP, SMTP, SASL, LDAP, SSH, MIME, SigV4 and HSTS are not used, and the tool-only advisories are not in the library. PC's FTP code is linked, and a redirect may lead to `ftp` or `ftps` by libcurl's default, but the FTP advisories need wildcard matching or a multi handle, which no caller sets.

## What is still open

- The advisory pages themselves were not read; the table rests on each entry's summary and details in `vuln.json`.
- CVE-2026-11586 has no applicable mitigation short of a curl bump other than setting `CURLWS_NOAUTOPONG` in `CURLOPT_WS_OPTIONS` and answering PINGs in the connector with a bound. Whether to do that or to lift the hold is the owner's decision (#813).
