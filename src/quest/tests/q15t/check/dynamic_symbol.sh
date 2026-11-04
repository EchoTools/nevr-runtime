#!/usr/bin/env bash
# usage: dynamic_symbol.sh <nm> <shared object> <symbol>
# Fails unless the symbol is defined in the dynamic symbols of the shared object.
set -euo pipefail
nm="$1"; library="$2"; symbol="$3"
symbols="$("$nm" -D --defined-only "$library")"
grep -q " ${symbol}\$" <<<"$symbols" || { echo "FAIL: ${symbol} is not in the dynamic symbols of an -fvisibility=hidden build"; exit 1; }
echo "${symbol} is a dynamic symbol of $(basename "$library")"
