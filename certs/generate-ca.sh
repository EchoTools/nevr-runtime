#!/usr/bin/env bash
# Generate the nEVR code signing CA hierarchy.
#
# Root CA key: the file NEVR_ROOT_CA_KEY names, outside the repository. Creating the root writes it there
# (and refuses to replace a key already at that path); --renew reads it.
# Intermediate and code-signing keys are local (gitignored).
#
# Usage:
#   NEVR_ROOT_CA_KEY=<file> ./certs/generate-ca.sh              # Full hierarchy from scratch
#   NEVR_ROOT_CA_KEY=<file> ./certs/generate-ca.sh --renew      # Re-issue intermediate + signing cert (root key reused)

set -euo pipefail

CERT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_VALIDITY=3650    # 10 years
INTER_VALIDITY=1825   # 5 years
SIGN_VALIDITY=825     # ~2.25 years (keeps under 825-day BR limit)

ROOT_KEY="${NEVR_ROOT_CA_KEY:-}"
[[ -n "$ROOT_KEY" ]] || { echo "ERROR: NEVR_ROOT_CA_KEY is not set: name the file for the root CA key (outside the repository)" >&2; exit 1; }

renew=false
[[ "${1:-}" == "--renew" ]] && renew=true

# --- Root CA ---
if [[ "$renew" == false ]]; then
    echo "==> Generating Root CA key + cert"
    [[ ! -e "$ROOT_KEY" ]] || { echo "ERROR: $ROOT_KEY already exists; refusing to replace the root CA key" >&2; exit 1; }

    openssl genrsa -out "$ROOT_KEY" 4096
    chmod 600 "$ROOT_KEY"
    echo "    Root CA key written to: $ROOT_KEY (NEVR_ROOT_CA_KEY)"

    # Generate self-signed root cert
    openssl req -new -x509 \
        -key "$ROOT_KEY" \
        -config "$CERT_DIR/root-ca.conf" \
        -days "$ROOT_VALIDITY" \
        -out "$CERT_DIR/root-ca.crt"

    echo "    Root CA cert: certs/root-ca.crt"
else
    echo "==> Renew mode: reusing Root CA key at $ROOT_KEY (NEVR_ROOT_CA_KEY)"
    [[ -r "$ROOT_KEY" ]] || { echo "ERROR: root CA key not readable at $ROOT_KEY (NEVR_ROOT_CA_KEY)" >&2; exit 1; }
fi

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
    -CAkey "$ROOT_KEY" \
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
