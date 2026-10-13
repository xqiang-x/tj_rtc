#!/bin/bash
#
# Prepare the BoringSSL static libraries required by the bundled xquic build.
# Clones google/boringssl on first use and compiles libssl.a/libcrypto.a.
# Mirrors the TLS backend flags used by xquic scripts/validate.sh.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BSSL_DIR="${ROOT_DIR}/rtc/libs/xquic/third_party/boringssl"
BSSL_REPO="${BORINGSSL_REPO:-https://github.com/google/boringssl.git}"
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

if [[ ! -f "${BSSL_DIR}/CMakeLists.txt" ]]; then
    git clone --depth 1 "${BSSL_REPO}" "${BSSL_DIR}"
fi

if [[ -f "${BSSL_DIR}/build/libssl.a" && -f "${BSSL_DIR}/build/libcrypto.a" ]]; then
    echo "BoringSSL static libraries already present: ${BSSL_DIR}/build"
    exit 0
fi

cmake -S "${BSSL_DIR}" -B "${BSSL_DIR}/build" \
    -DBUILD_SHARED_LIBS=0 \
    -DCMAKE_C_FLAGS="-fPIC" \
    -DCMAKE_CXX_FLAGS="-fPIC"
CMAKE_BUILD_PARALLEL_LEVEL="${JOBS}" \
    cmake --build "${BSSL_DIR}/build" --target ssl crypto

echo "BoringSSL ready: ${BSSL_DIR}/build/libssl.a ${BSSL_DIR}/build/libcrypto.a"