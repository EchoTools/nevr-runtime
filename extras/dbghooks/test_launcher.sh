#!/bin/bash
# Quick test script for EchoVRLauncher with hash capture

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GAME_DIR="$SCRIPT_DIR/../ready-at-dawn-echo-arena/bin/win10"
BUILD_DIR="$SCRIPT_DIR/../build/mingw-release"

echo "==================================================="
echo "  EchoVR Hash Discovery Test"
echo "==================================================="

# Check build artifacts
if [ ! -f "$BUILD_DIR/bin/EchoVRLauncher.exe" ]; then
    echo "ERROR: EchoVRLauncher.exe not found"
    echo "Build it with: cmake --build build/mingw-release --target EchoVRLauncher"
    exit 1
fi

if [ ! -f "$BUILD_DIR/bin/DbgHooks.dll" ]; then
    echo "ERROR: DbgHooks.dll not found"
    echo "Build it with: cmake --build build/mingw-release --target DbgHooks"
    exit 1
fi

if [ ! -f "$GAME_DIR/echovr.exe" ]; then
    echo "ERROR: echovr.exe not found at $GAME_DIR"
    exit 1
fi

echo "✓ Build artifacts found"
echo "✓ Game executable found"
echo ""

# Copy artifacts to game directory
echo "Copying build artifacts to game directory..."
cp "$BUILD_DIR/bin/EchoVRLauncher.exe" "$GAME_DIR/"
cp "$BUILD_DIR/bin/DbgHooks.dll" "$GAME_DIR/"
echo "✓ Files copied"
echo ""

# Backup old log if exists
if [ -f "$GAME_DIR/hash_discovery.log" ]; then
    TIMESTAMP=$(date +%Y%m%d_%H%M%S)
    mv "$GAME_DIR/hash_discovery.log" "$GAME_DIR/hash_discovery_backup_$TIMESTAMP.log"
    echo "✓ Backed up old log to hash_discovery_backup_$TIMESTAMP.log"
fi

# Change to game directory
cd "$GAME_DIR"

echo ""
echo "==================================================="
echo "  Launching Echo VR with early DLL injection"
echo "==================================================="
echo ""
echo "Expected behavior:"
echo "  1. Game spawns in suspended state"
echo "  2. DbgHooks.dll injected"
echo "  3. Game resumes with hooks active"
echo "  4. SNS message registrations captured at startup"
echo ""
echo "Press Ctrl+C to stop the game"
echo ""
echo "Running: wine EchoVRLauncher.exe echovr.exe DbgHooks.dll -noovr -windowed -level mpl_lobby_b2"
echo ""
echo "---------------------------------------------------"

# Run the launcher
wine EchoVRLauncher.exe echovr.exe DbgHooks.dll -noovr -windowed -level mpl_lobby_b2

echo ""
echo "---------------------------------------------------"
echo ""
echo "==================================================="
echo "  Test Complete"
echo "==================================================="

# Check log file
if [ -f "$GAME_DIR/hash_discovery.log" ]; then
    TOTAL_LINES=$(wc -l < "$GAME_DIR/hash_discovery.log")
    SNS_COUNT=$(grep -c "\[SNS_COMPLETE\]" "$GAME_DIR/hash_discovery.log" || true)
    CSYM_COUNT=$(grep -c "\[CSymbol64_Hash\]" "$GAME_DIR/hash_discovery.log" || true)
    
    echo ""
    echo "Log analysis:"
    echo "  Total lines: $TOTAL_LINES"
    echo "  SNS messages: $SNS_COUNT (expected: ~87)"
    echo "  Replicated variables: $CSYM_COUNT (expected: ~15,709)"
    echo ""
    
    if [ $SNS_COUNT -gt 50 ]; then
        echo "✅ SUCCESS: Captured significant SNS messages!"
    elif [ $SNS_COUNT -gt 0 ]; then
        echo "⚠️  PARTIAL: Some SNS messages captured, but fewer than expected"
    else
        echo "❌ FAILED: No SNS messages captured"
        echo ""
        echo "Possible issues:"
        echo "  - Hooks installed too late (check log for [HOOK] messages)"
        echo "  - Game cached hashes differently"
        echo "  - Injection failed silently"
    fi
    
    echo ""
    echo "First few SNS captures:"
    grep "\[SNS_COMPLETE\]" "$GAME_DIR/hash_discovery.log" | head -n 5
    
else
    echo "❌ ERROR: hash_discovery.log not created"
    echo ""
    echo "Possible issues:"
    echo "  - DLL not loaded"
    echo "  - Hooks not installed"
    echo "  - File permission issues"
fi

echo ""
echo "Log file: $GAME_DIR/hash_discovery.log"
echo ""
echo "Next steps:"
echo "  1. Review the log file for [SNS_COMPLETE] entries"
echo "  2. Run parse_hash_log.py to generate C++ headers"
echo "  3. Integrate into nevr-server"
