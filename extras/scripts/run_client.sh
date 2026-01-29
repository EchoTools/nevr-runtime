#!/bin/bash
## Run Echo VR as a client (windowed mode, no VR)
wine ./echovr/bin/win10/echovr.exe -noovr -windowed 2>&1 | grep -v vkd3d:
