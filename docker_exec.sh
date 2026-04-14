#!/bin/bash
set -e
CLANG_DIR=$(ls -d /opt/ddk/clang/clang-*)
KMI=$(ls /opt/ddk/kdir/)
KDIR="/opt/ddk/kdir/${KMI}"
TAG="$KMI"
export PATH="${CLANG_DIR}/bin:${PATH}"
FAIL=0
BUILD_DIR="/tmp/build"
mkdir -p "$BUILD_DIR"
MAKEFILE_CONTENT=""
for ENTRY in "$@"; do
    SRC_FILE="${ENTRY%%:*}"
    MOD_NAME="${ENTRY##*:}"
    MAKEFILE_CONTENT="${MAKEFILE_CONTENT}obj-m += ${MOD_NAME}.o
"
    cp "/src/${SRC_FILE}" "${BUILD_DIR}/${MOD_NAME}.c"
done
printf '%s' "$MAKEFILE_CONTENT" > "${BUILD_DIR}/Makefile"
if make -C "${KDIR}" -j "$(nproc)" M="${BUILD_DIR}" ARCH=arm64 LLVM=1 LLVM_IAS=1 modules; then
    for ENTRY in "$@"; do
        MOD_NAME="${ENTRY##*:}"
        llvm-strip -d "${BUILD_DIR}/${MOD_NAME}.ko"
        cp "${BUILD_DIR}/${MOD_NAME}.ko" "/out_${MOD_NAME}/${TAG}.ko"
        echo "OK: ${MOD_NAME}@${TAG}"
    done
else
    FAIL=1
    for ENTRY in "$@"; do
        MOD_NAME="${ENTRY##*:}"
        if [ -f "${BUILD_DIR}/${MOD_NAME}.ko" ]; then
            llvm-strip -d "${BUILD_DIR}/${MOD_NAME}.ko"
            cp "${BUILD_DIR}/${MOD_NAME}.ko" "/out_${MOD_NAME}/${TAG}.ko"
            echo "OK: ${MOD_NAME}@${TAG}"
        else
            echo "FAIL: ${MOD_NAME}@${TAG}"
        fi
    done
fi
exit $FAIL
