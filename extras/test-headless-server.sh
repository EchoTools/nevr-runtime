#!/bin/bash
cp -v dist/nevr-server*/* echovr/bin/win10
cd echovr/bin/win10 && wine ./echovr.exe -noovr -server -headless -noconsole -fixedtimestep -timestep 120
