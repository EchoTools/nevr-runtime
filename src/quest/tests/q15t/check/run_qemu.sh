#!/usr/bin/env bash
# usage: run_qemu.sh <arm64 program>
# Runs the freestanding arm64 thunk program under qemu-aarch64. Q15T_SKIP_QEMU=1 skips it, and says so.
set -euo pipefail
program="$1"
if command -v qemu-aarch64 >/dev/null 2>&1; then
  qemu-aarch64 "$program" || { echo "FAIL: $(basename "$program")"; exit 1; }
elif [ "${Q15T_SKIP_QEMU:-0}" = 1 ]; then
  echo "SKIPPED (Q15T_SKIP_QEMU=1): qemu-aarch64 not found, the thunk was not run"
else
  echo "FAIL: qemu-aarch64 not found (Q15T_SKIP_QEMU=1 to skip explicitly)"
  exit 1
fi
