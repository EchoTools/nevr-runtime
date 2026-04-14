#!/bin/bash
# Code sign Windows PE binaries using osslsigncode.
#
# Usage: sign.sh <file> [file ...]
#
# Environment variables:
#   CODESIGN_CERT   - Path to certificate (.crt/.pem)
#   CODESIGN_KEY    - Path to private key (.key/.pem)
#   CODESIGN_PFX    - Path to PKCS#12 bundle (.pfx/.p12), used instead of cert+key
#   CODESIGN_PASS   - Password for PFX file (if encrypted)
#   CODESIGN_TSURL  - Timestamp server URL (optional)
#   CODESIGN_SKIP   - Set to 1 to skip signing entirely
#
# Falls back to cmake/codesign/selfsigned.crt/key if no env vars set.
# If no credentials are available, skips signing with a warning (non-fatal).

set -euo pipefail

if [[ "${CODESIGN_SKIP:-}" == "1" ]]; then
    echo "Code signing skipped (CODESIGN_SKIP=1)"
    exit 0
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Resolve signing credentials
if [[ -n "${CODESIGN_PFX:-}" ]]; then
    SIGN_ARGS=(-pkcs12 "$CODESIGN_PFX")
    [[ -n "${CODESIGN_PASS:-}" ]] && SIGN_ARGS+=(-pass "$CODESIGN_PASS")
elif [[ -n "${CODESIGN_CERT:-}" && -n "${CODESIGN_KEY:-}" ]]; then
    SIGN_ARGS=(-certs "$CODESIGN_CERT" -key "$CODESIGN_KEY")
elif [[ -f "$SCRIPT_DIR/selfsigned.crt" && -f "$SCRIPT_DIR/selfsigned.key" ]]; then
    SIGN_ARGS=(-certs "$SCRIPT_DIR/selfsigned.crt" -key "$SCRIPT_DIR/selfsigned.key")
elif [[ -f "$SCRIPT_DIR/../../certs/chain.pem" && -f "$SCRIPT_DIR/../../certs/code-signing.key" ]]; then
    SIGN_ARGS=(-certs "$SCRIPT_DIR/../../certs/chain.pem" -key "$SCRIPT_DIR/../../certs/code-signing.key")
else
    echo "WARNING: No signing credentials found, skipping code signing." >&2
    echo "Set CODESIGN_PFX or CODESIGN_CERT+CODESIGN_KEY, or generate selfsigned certs." >&2
    exit 0
fi

# Timestamp server
if [[ -n "${CODESIGN_TSURL:-}" ]]; then
    SIGN_ARGS+=(-ts "$CODESIGN_TSURL")
fi

# Sign each file
for file in "$@"; do
    if [[ ! -f "$file" ]]; then
        echo "Warning: $file not found, skipping" >&2
        continue
    fi
    echo "Signing: $file"
    osslsigncode sign \
        "${SIGN_ARGS[@]}" \
        -n "nEVR Runtime" \
        -h sha256 \
        -in "$file" \
        -out "$file.signed" && \
    mv "$file.signed" "$file"
done
