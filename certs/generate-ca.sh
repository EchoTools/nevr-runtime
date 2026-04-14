#!/usr/bin/env bash
# Generate the nEVR code signing CA hierarchy.
#
# Root CA key is stored in pass (services/echovrce/code-signing-root-ca-key).
# Intermediate and code-signing keys are local (gitignored).
#
# Usage:
#   ./certs/generate-ca.sh              # Full hierarchy from scratch
#   ./certs/generate-ca.sh --renew      # Re-issue intermediate + signing cert (root stays in pass)

set -euo pipefail

CERT_DIR="$(cd "$(dirname "$0")" && pwd)"
PASS_PATH="services/echovrce/code-signing-root-ca-key"
ROOT_VALIDITY=3650    # 10 years
INTER_VALIDITY=1825   # 5 years
SIGN_VALIDITY=825     # ~2.25 years (keeps under 825-day BR limit)

renew=false
[[ "${1:-}" == "--renew" ]] && renew=true

# --- Root CA ---
if [[ "$renew" == false ]]; then
    echo "==> Generating Root CA key + cert"

    # Generate key to a temp file, store in pass, then shred
    root_key_tmp=$(mktemp)
    trap 'shred -u "$root_key_tmp" 2>/dev/null; rm -f "$root_key_tmp"' EXIT

    openssl genrsa -out "$root_key_tmp" 4096

    # Store in pass
    pass insert -m "$PASS_PATH" < "$root_key_tmp"
    echo "    Root CA key stored in pass at: $PASS_PATH"

    # Generate self-signed root cert
    openssl req -new -x509 \
        -key "$root_key_tmp" \
        -config "$CERT_DIR/root-ca.conf" \
        -days "$ROOT_VALIDITY" \
        -out "$CERT_DIR/root-ca.crt"

    echo "    Root CA cert: certs/root-ca.crt"
else
    echo "==> Renew mode: reusing Root CA from pass"
    if ! pass show "$PASS_PATH" > /dev/null 2>&1; then
        echo "ERROR: Root CA key not found in pass at $PASS_PATH" >&2
        exit 1
    fi
fi

# Extract root key from pass for signing (temp only)
root_key_tmp="${root_key_tmp:-$(mktemp)}"
trap 'shred -u "$root_key_tmp" 2>/dev/null; rm -f "$root_key_tmp"' EXIT
pass show "$PASS_PATH" > "$root_key_tmp"

# --- Intermediate CA ---
echo "==> Generating Intermediate CA key + cert"

openssl genrsa -out "$CERT_DIR/intermediate-ca.key" 4096
chmod 600 "$CERT_DIR/intermediate-ca.key"

openssl req -new \
    -key "$CERT_DIR/intermediate-ca.key" \
    -config "$CERT_DIR/intermediate-ca.conf" \
    -out "$CERT_DIR/intermediate-ca.csr"

openssl x509 -req \
    -in "$CERT_DIR/intermediate-ca.csr" \
    -CA "$CERT_DIR/root-ca.crt" \
    -CAkey "$root_key_tmp" \
    -CAcreateserial \
    -days "$INTER_VALIDITY" \
    -extfile "$CERT_DIR/intermediate-ca.conf" \
    -extensions v3_intermediate \
    -out "$CERT_DIR/intermediate-ca.crt"

rm -f "$CERT_DIR/intermediate-ca.csr"
echo "    Intermediate CA cert: certs/intermediate-ca.crt"

# --- Code Signing cert ---
echo "==> Generating Code Signing key + cert"

openssl genrsa -out "$CERT_DIR/code-signing.key" 4096
chmod 600 "$CERT_DIR/code-signing.key"

openssl req -new \
    -key "$CERT_DIR/code-signing.key" \
    -config "$CERT_DIR/code-signing.conf" \
    -out "$CERT_DIR/code-signing.csr"

openssl x509 -req \
    -in "$CERT_DIR/code-signing.csr" \
    -CA "$CERT_DIR/intermediate-ca.crt" \
    -CAkey "$CERT_DIR/intermediate-ca.key" \
    -CAcreateserial \
    -days "$SIGN_VALIDITY" \
    -extfile "$CERT_DIR/intermediate-ca.conf" \
    -extensions v3_codesign \
    -out "$CERT_DIR/code-signing.crt"

rm -f "$CERT_DIR/code-signing.csr"
echo "    Code Signing cert: certs/code-signing.crt"

# --- Build chain file ---
echo "==> Building certificate chain"
cat "$CERT_DIR/code-signing.crt" \
    "$CERT_DIR/intermediate-ca.crt" \
    "$CERT_DIR/root-ca.crt" \
    > "$CERT_DIR/chain.pem"

echo "    Full chain: certs/chain.pem"

# --- Verify ---
echo "==> Verifying chain"
openssl verify -CAfile "$CERT_DIR/root-ca.crt" \
    -untrusted "$CERT_DIR/intermediate-ca.crt" \
    "$CERT_DIR/code-signing.crt"

echo ""
echo "Done. Sign DLLs with: just sign"
