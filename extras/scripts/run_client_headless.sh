#!/bin/bash
## Run Echo VR client in virtual X server (headless)
## Uses Xvfb to create a virtual display so the game runs without showing windows

# Kill any existing Xvfb on display :99 (cleanup from previous runs)
pkill -f "Xvfb :99" 2>/dev/null

# Start Xvfb on display :99 with 1920x1080 resolution, 24-bit color
Xvfb :99 -screen 0 1920x1080x24 -nolisten tcp -noreset &
XVFB_PID=$!

# Give Xvfb time to start
sleep 2

# Export DISPLAY to point to the virtual X server
export DISPLAY=:99

# Run the client with Wine on the virtual display
echo "[INFO] Starting Echo VR client on virtual display :99"
echo "[INFO] Xvfb PID: $XVFB_PID"
wine ./echovr/bin/win10/echovr.exe -noovr -windowed -gametype echo_combat_private -mp 2>&1 | grep -v vkd3d:

# Cleanup: Kill Xvfb when the game exits
echo "[INFO] Game exited, stopping Xvfb (PID $XVFB_PID)"
kill $XVFB_PID 2>/dev/null
