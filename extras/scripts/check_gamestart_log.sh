#!/bin/bash
# Quick diagnostic - check if gamestart_debug.log is being created

GAME_DIR="/home/andrew/src/nevr-server/ready-at-dawn-echo-arena/bin/win10"
LOG_FILE="$GAME_DIR/gamestart_debug.log"

echo "=== GameStart Hook Diagnostic ==="
echo ""
echo "Checking for log file: $LOG_FILE"

if [ -f "$LOG_FILE" ]; then
    echo "✓ Log file EXISTS"
    echo ""
    echo "Last modified:"
    ls -lh "$LOG_FILE"
    echo ""
    echo "File contents:"
    cat "$LOG_FILE"
else
    echo "✗ Log file DOES NOT EXIST"
    echo ""
    echo "This means InitializeGameStartHooks() either:"
    echo "  1. Is not being called at all"
    echo "  2. Failed before creating the log file"
    echo "  3. Failed on fopen() due to permissions/path"
fi

echo ""
echo "Checking other DbgHooks log files:"
ls -lh "$GAME_DIR"/*.log 2>/dev/null || echo "No .log files found"

echo ""
echo "Checking if DbgHooks.dll exists:"
ls -lh "$GAME_DIR/DbgHooks.dll"

echo ""
echo "Checking for GameStartHooks strings in DLL:"
strings "$GAME_DIR/DbgHooks.dll" | grep "GameStartHooks" | head -5
