#!/bin/sh
export DISPLAY=":101"
export WINEPREFIX="/home/andrew/src/nevr-runtime/.wineprefix"

wine echovr/bin/win10/echovr.exe -noovr -windowed -mp 2>&1 | grep -v vkd3d
