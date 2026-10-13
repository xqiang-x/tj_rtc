#!/usr/bin/env bash
# Generate the ECDSA P-256 certificate used by moq_gateway --wt.
#
# Chromium's serverCertificateHashes requires: ECDSA P-256, validity <= 14
# days, leaf (non-CA) certificate. Usage:
#   ./gen_cert.sh [--force]
set -euo pipefail
cd "$(dirname "$0")"

CERT_DIR=certs
CERT="$CERT_DIR/server.crt"
KEY="$CERT_DIR/server.key"
FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

if command -v openssl >/dev/null 2>&1; then :; else
  echo "error: openssl not found" >&2
  exit 1
fi

mkdir -p "$CERT_DIR"

if [ -f "$CERT" ] && [ -f "$KEY" ] && [ "$FORCE" -eq 0 ]; then
  echo "certificate exists: $CERT (use --force to regenerate)"
else
  openssl ecparam -name prime256v1 -genkey -noout -out "$KEY"
  openssl req -new -x509 -key "$KEY" -out "$CERT" -days 14 \
    -subj "/CN=localhost" \
    -addext "basicConstraints=critical,CA:FALSE" \
    -addext "keyUsage=critical,digitalSignature" \
    -addext "extendedKeyUsage=serverAuth" \
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" \
    2>/dev/null
  echo "generated $CERT / $KEY (ECDSA P-256, 14 days)"
fi

FP=$(openssl x509 -in "$CERT" -outform der | openssl dgst -sha256 -binary | openssl base64)
echo "$FP" > "$CERT_DIR/fingerprint.txt"
echo "fingerprint: sha-256/$FP"