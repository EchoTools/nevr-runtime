make dist && rsync -r dist/nevr-server-*/* ./echovr/bin/win10 && wine ./echovr/bin/win10/echovr_launcher.exe -noovr -windowed -gametype echo_combat_private -mp 2>&1 | grep -v vkd3d:
