# Issues - Testing Integration & Player Join Debugging

*Problems, gotchas, and unexpected behaviors encountered*

---


## [2026-02-05T16:59:22-06:00] Task 2: evr-mcp Server Mode Failure

### Problem
evr-mcp_echovr_start cannot launch game in server mode with backup DLLs. Timeout after 90s.

### Symptoms
- evr-mcp_echovr_start with gametype="echo_arena", headless=true, wait_ready=true → TIMEOUT
- Manual wine launch succeeds but starts client mode (no HTTP API server)
- Port 6721 not listening even though -httpport 6721 flag accepted

### Impact
- Cannot test ServerDB registration with backup DLLs
- Cannot capture server-side events via HTTP API
- Cannot verify /session endpoint behavior

### Possible Causes
1. evr-mcp tool may require NEVR-specific DLLs (gameserverlegacy.dll, etc.)
2. Backup DLLs missing server mode initialization
3. Game server mode requires additional flags or environment setup
4. wine environment issue preventing HTTP server socket creation

### Evidence
- task-2-backup-events.json: Documented client mode launch
- task-2-backup-state.json: HTTP API unavailable
- Game log shows no server initialization messages

### Blocker Status
**HIGH**: Prevents proper baseline testing of backup configuration. Cannot compare with NEVR DLL behavior if server mode doesn't work.
