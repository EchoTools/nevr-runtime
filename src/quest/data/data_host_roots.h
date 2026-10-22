/* The trust anchor for the data host (docs/adr/0011-quest-game-data-first-run.md): the one root the host's certificate
 * chain ends in, not the device's whole store. The sha256 pin (plan.h) is the integrity check; TLS keeps the download
 * private and stable.
 *
 * Measured 2026-10-22 with `openssl s_client -connect mia.cdn.echo.taxi:443 -showcerts`: leaf CN=4d2e64e3.sni.cloudflaressl.com
 * (issuer GTS WE1), then GTS WE1 (issuer GTS Root R4), then GTS Root R4 as the cross-signature issued by GlobalSign
 * Root CA. The self-signed GTS Root R4 below is the anchor: `curl --cacert` with only this certificate fetched
 * `https://mia.cdn.echo.taxi/_data.zip` (HTTP 206, ssl_verify_result 0). It expires 2036-06-22. A host that moves to a
 * chain under another root makes every fetch fail closed (the floor message); the pin list is then edited here.
 */
#pragma once

namespace nevr_quest::data {

// SHA-256 of the DER of the certificate below.
inline constexpr const char kDataHostRootSha256[] = "349dfa4058c5e263123b398ae795573c4e1313c83fe68f93556cd5e8031b3c7d";

inline constexpr char kDataHostRootsPem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD\n"
    "VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG\n"
    "A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw\n"
    "WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz\n"
    "IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi\n"
    "AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi\n"
    "QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR\n"
    "HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW\n"
    "BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D\n"
    "9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8\n"
    "p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD\n"
    "-----END CERTIFICATE-----\n";

}  // namespace nevr_quest::data
