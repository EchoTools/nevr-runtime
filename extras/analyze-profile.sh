#!/usr/bin/env zsh
set -euo pipefail

# Analyze perf data captured by profile-server.sh.
#
# Usage:
#   ./extras/analyze-profile.sh /var/tmp/nevr-profiles/perf-ts120-*.data

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 <perf.data>"
    echo ""
    echo "Available profiles:"
    ls -lt /var/tmp/nevr-profiles/perf-*.data 2>/dev/null || echo "  (none)"
    exit 1
fi

PERF_DATA="$1"

if [[ ! -f "$PERF_DATA" ]]; then
    echo "ERROR: $PERF_DATA not found"
    exit 1
fi

echo "=== Top Functions (flat, no children) ==="
echo ""
perf report -i "$PERF_DATA" --no-children --percent-limit 0.5 --stdio 2>/dev/null | head -80

echo ""
echo "=== Top Functions (with callers) ==="
echo ""
perf report -i "$PERF_DATA" --percent-limit 2 --stdio 2>/dev/null | head -120

echo ""
echo "=== Wine/ntdll hotspots ==="
echo ""
perf report -i "$PERF_DATA" --no-children --percent-limit 0.3 --stdio 2>/dev/null | \
    grep -iE 'ntdll|wine|sleep|switch|thread|wait|qpc|perf|timer|spin|lock|critical' | head -30

echo ""
echo "=== Full interactive report ==="
echo "  perf report -i $PERF_DATA --no-children"
