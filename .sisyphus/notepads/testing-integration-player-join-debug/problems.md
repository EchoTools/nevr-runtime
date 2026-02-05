# Problems - Testing Integration & Player Join Debugging

*Unresolved blockers requiring attention*

---


## [2026-02-05T23:03:00Z] BLOCKER: Server Mode Cannot Start

### Problem Statement
NEITHER backup DLLs NOR current build DLLs can start the game in server mode via evr-mcp.

### Evidence
- Task 2: Backup DLLs → client mode only
- Task 3: Current DLLs → client mode only  
- Both configurations: evr-mcp_echovr_start TIMEOUT (90s)
- Both configurations: Game launches but connects to remote server (172.125.239.112:6794)
- HTTP API error: "Endpoint is restricted in this match type" (err_code -6)

### Impact
**CRITICAL**: Cannot execute the original test plan. Cannot capture server-side errors if server mode doesn't start.

### Root Cause Analysis Needed
1. What flags/config are required to force server mode?
2. Does evr-mcp tool have correct server mode startup logic?
3. Is there missing environment setup?
4. Does NEVR server require specific Nakama backend running first?

### Recommended Next Steps
1. Skip Task 4 (no comparison possible - both fail)
2. Task 5: Analyze server startup code in nevr-server
3. Investigate evr-test-harness server startup mechanism
4. Check if Nakama backend is required dependency
