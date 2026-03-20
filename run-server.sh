#!/usr/bin/env zsh
set -euo pipefail

SERVER_DIR="/mnt/games/CustomLibrary/ready-at-dawn-echo-arena"

pushd "$SERVER_DIR" >/dev/null
wine ./bin/win10/echovr.exe -noovr -server -headless -noconsole -timestep 120 -config-path ./_local/l.config.json
popd >/dev/null
