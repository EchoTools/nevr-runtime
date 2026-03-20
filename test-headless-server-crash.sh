#!/bin/bash
set -o pipefail

cd /home/andrew/src/nevr-server

if [ ! -L "echovr" ]; then
    echo "ERROR LINK NOT FOUND -- ./echovr -> /mnt/games/CustomLibrary/ready-at-dawn-echo-arena"
    exit 1
fi

cd echovr/bin/win10

WINEDEBUG=-all wine ./echovr.exe -noovr -server -headless -noconsole -fixedtimestep -timestep 120 >/dev/null 2>&1 &
WINE_PID=$!

for i in {1..300}; do
    if ! kill -0 $WINE_PID 2>/dev/null; then
        echo "FAILED"
        exit 1
    fi
    sleep 0.1
done

pkill -9 echovr.exe 2>/dev/null
echo "SUCCESS"
exit 0
