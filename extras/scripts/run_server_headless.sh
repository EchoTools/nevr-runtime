#!/bin/bash
## Run echo as a headless server
wine ./echovr/bin/win10/echovr.exe -noovr -server -headless -noconsole 2>&1 | grep -v vkd3d:
