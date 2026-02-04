#!/bin/bash
cp -v dist/nevr-server*/* echovr/bin/win10
cd echovr/bin/win10 && wine ./echovr.exe -noovr -windowed -gametype echo_arena_private -mp
