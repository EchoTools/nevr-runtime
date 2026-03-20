# Frida Hook Usage

Install Frida:
```bash
pip install frida-tools
```

Run against Echo VR:
```bash
# Launch and attach immediately
frida -l scripts/dump_message_hashes.js -f /path/to/echovr.exe --no-pause

# Or attach to running process
frida -l scripts/dump_message_hashes.js echovr.exe
```

Output will show all registered message types at startup:
```
[MSG_REGISTRY] 0x59e4c5ea6e01083b = "ActualName" (flags=0x1)
```

Frida guarantees hooks install before any game code runs.
