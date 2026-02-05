# Testing Integration & Player Join Debugging

## TL;DR

> **Quick Summary**: Set up A/B testing infrastructure using evr-test-harness to diagnose why the legacygameserver DLL errors when a player joins, compared to the working backup DLLs. The approach involves swapping DLLs atomically, running standardized test cases, capturing logs, and comparing to identify the exact failure point.
> 
> **Deliverables**:
> - DLL swap tooling/script for evr-test-harness
> - Player join test case that captures error logs
> - A/B comparison of backup vs current build
> - Root cause identification with fix recommendations
> 
> **Estimated Effort**: Medium
> **Parallel Execution**: NO - sequential (each test requires clean state)
> **Critical Path**: Task 1 → Task 2 → Task 3 → Task 4 → Task 5 → Task 6

---

## Context

### Original Request
User wants to set up testing abilities in nevr-server using evr-test-harness, with the first task being to diagnose why the legacygameserver code errors when a player joins, while the backup DLLs work correctly.

### Interview Summary
**Key Discussions**:
- Error Type: Log error message (not a crash) when player joins
- Working Backup: `/mnt/games/CustomLibrary/echovr-34.4.631547.1/bin/win10/pnsradgameserver.dll` (112KB, Jan 7)
- Current Broken: `/home/andrew/src/nevr-server/dist/nevr-server-v3.2.0+30.83a0518/` (18MB, Feb 4)
- Testing Mode: Real game instances via evr-test-harness (no mocks)
- **Player Join**: ERROR occurs during game startup/ServerDB registration (NOT via HTTP API - there is no HTTP join endpoint)
- Focus: Diagnose current legacy gameserver issue specifically

**Research Findings**:
- evr-test-harness uses symlink: `ready-at-dawn-echo-arena` → `/mnt/games/CustomLibrary/ready-at-dawn-echo-arena`
- Recent commits (Feb 4) added WebSocket client and LobbyEntrantsV3 handler - claimed to fix player join
- Test harness has 21 MCP tools including `echovr_events` for log parsing
- **CRITICAL**: Current game directory has HYBRID DLL state (mix of backup + current build) - this may BE the bug
- **CRITICAL**: No HTTP endpoint exists for player join. Player joins require WebSocket/Nakama protocol.
  - The error likely occurs during: (a) game startup, (b) ServerDB registration, or (c) Nakama session creation
  - We will capture errors during game startup/running to identify the issue

### Metis Review
**Identified Gaps** (addressed in plan):
1. **No atomic DLL swap**: Plan includes proper swap procedure
2. **Hybrid DLL state**: Plan requires ALL NEVR DLLs from same source (game core DLLs untouched)
3. **Player join mechanism**: UPDATED - No HTTP join endpoint exists; capture errors during game startup/ServerDB registration
4. **MD5 verification missing**: Added to acceptance criteria
5. **gameserverlegacy.dll confusion**: Clarified - backup doesn't have it, only current build does

### DLL Inventory (CRITICAL for Atomic Swap)

**NEVR-Specific DLLs (these are what we swap)**:
| DLL | Backup (Jan 7) | Current Build (Feb 4) |
|-----|----------------|----------------------|
| `pnsradgameserver.dll` | ✅ 112KB | ✅ 18MB |
| `dbgcore.dll` | ✅ 181KB | ✅ 2.9MB |
| `gameserverlegacy.dll` | ❌ N/A | ✅ 855KB |
| `gamepatcheslegacy.dll` | ❌ N/A | ✅ 261KB |
| `telemetryagent.dll` | ❌ N/A | ✅ 19MB |

**Game Core DLLs (DO NOT TOUCH)**:
- `pnsrad.dll`, `pnsovr.dll`, `pnsdemo.dll`, `pnsradmatchmaking.dll`
- `OculusSpatializerWwise.dll`, `BugSplat*.dll`, `amd_ags_x64.dll`
- `scripts/*.dll` (90+ script DLLs)

**Atomic Swap Strategy**:
- **Config A (Backup)**: Copy `pnsradgameserver.dll`, `dbgcore.dll` from backup; REMOVE `gameserverlegacy.dll`, `gamepatcheslegacy.dll`, `telemetryagent.dll`
- **Config B (Current)**: Copy ALL 5 DLLs from current dist

---

## Work Objectives

### Core Objective
Diagnose why the legacygameserver DLL causes errors on player join by running A/B tests comparing backup (working) vs current (broken) DLLs, capturing logs, and identifying the exact failure point.

### Concrete Deliverables
1. DLL swap script/tool for test harness
2. Player join test case with log capture
3. A/B test results documenting differences
4. Root cause analysis with fix recommendations

### Definition of Done
- [ ] A/B test executed with both configurations
- [ ] Error message from broken config captured verbatim
- [ ] Logs compared and differences documented
- [ ] Root cause identified with actionable fix

### Must Have
- Atomic DLL swap (ALL DLLs from same source)
- Log capture including error events
- Clean state between tests
- MD5 verification of installed DLLs

### Must NOT Have (Guardrails)
- ❌ Do NOT mix DLLs from different builds (hybrid state)
- ❌ Do NOT run multiple tests without clearing game state
- ❌ Do NOT modify any source code during diagnosis (read-only investigation)
- ❌ Do NOT use mocks or simulations - real game only
- ❌ Do NOT create new DLL builds - only compare existing

---

## Verification Strategy (MANDATORY)

> **UNIVERSAL RULE: ZERO HUMAN INTERVENTION**
>
> ALL tasks in this plan MUST be verifiable WITHOUT any human action.
> This is NOT conditional — it applies to EVERY task.

### Test Decision
- **Infrastructure exists**: YES (evr-test-harness has 44 integration tests)
- **Automated tests**: Tests-after (write test after fixing)
- **Framework**: Go tests in evr-test-harness

### Agent-Executed QA Scenarios (MANDATORY — ALL tasks)

> Whether TDD is enabled or not, EVERY task MUST include Agent-Executed QA Scenarios.
> These describe how the executing agent DIRECTLY verifies the deliverable.

**Verification Tool by Deliverable Type:**

| Type | Tool | How Agent Verifies |
|------|------|-------------------|
| **DLL files** | Bash (md5sum, ls -la) | Check size, hash, existence |
| **Game state** | evr-mcp_echovr_state | Query HTTP API for status |
| **Log events** | evr-mcp_echovr_events | Parse events for errors |
| **Process** | evr-mcp_echovr_start/stop | Start/stop game instances |

---

## Execution Strategy

### Sequential Execution (NO parallelism)

```
Task 1: Verify current environment
    ↓
Task 2: Execute Test A (backup DLLs)
    ↓
Task 3: Execute Test B (current build DLLs)
    ↓
Task 4: Compare logs and identify differences
    ↓
Task 5: Trace root cause in code
    ↓
Task 6: Document findings and recommendations
```

### Dependency Matrix

| Task | Depends On | Blocks | Can Parallelize With |
|------|------------|--------|---------------------|
| 1 | None | 2, 3 | None |
| 2 | 1 | 4 | None |
| 3 | 1, 2 | 4 | None |
| 4 | 2, 3 | 5 | None |
| 5 | 4 | 6 | None |
| 6 | 5 | None | None |

---

## TODOs

- [x] 1. Verify Test Environment and Document Current State

  **What to do**:
  - Create evidence directory: `mkdir -p .sisyphus/evidence`
  - Check evr-test-harness is built and ready
  - Document current DLL state in game directory
  - Identify which DLLs are from backup vs current build
  - Verify game directory symlink is correct
  - Calculate MD5 hashes of all relevant DLLs

  **Must NOT do**:
  - Do NOT modify any files yet
  - Do NOT start game instances

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: Simple verification tasks, file inspection only
  - **Skills**: [`playwright`]
    - `playwright`: Not needed but required parameter

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Sequential
  - **Blocks**: Task 2, 3
  - **Blocked By**: None

  **References**:
  - `/home/andrew/src/evr-test-harness/internal/echovr/config.go` - Game directory constants
  - `/mnt/games/CustomLibrary/echovr-34.4.631547.1/bin/win10/` - Backup DLLs location
  - `/home/andrew/src/nevr-server/dist/nevr-server-v3.2.0+30.83a0518/` - Current build location
  - `/mnt/games/CustomLibrary/ready-at-dawn-echo-arena/bin/win10/` - Active game directory

  **Acceptance Criteria**:

  **Agent-Executed QA Scenarios:**

  ```
  Scenario: Create evidence directory
    Tool: Bash
    Preconditions: None
    Steps:
      1. mkdir -p /home/andrew/src/nevr-server/.sisyphus/evidence
      2. Assert: Directory exists
    Expected Result: Evidence directory created
    Evidence: N/A (setup step)

  Scenario: Verify test harness binary exists
    Tool: Bash
    Preconditions: None
    Steps:
      1. ls -la /home/andrew/src/evr-test-harness/bin/evr-mcp
      2. Assert: File exists and is executable
    Expected Result: Binary exists with execute permissions
    Evidence: Command output captured

  Scenario: Document game directory symlink
    Tool: Bash
    Preconditions: None
    Steps:
      1. readlink -f /home/andrew/src/evr-test-harness/ready-at-dawn-echo-arena
      2. Assert: Points to /mnt/games/CustomLibrary/ready-at-dawn-echo-arena
    Expected Result: Symlink resolves correctly
    Evidence: Path captured

  Scenario: Catalog current DLLs in game directory
    Tool: Bash
    Preconditions: None
    Steps:
      1. ls -la /mnt/games/CustomLibrary/ready-at-dawn-echo-arena/bin/win10/*.dll
      2. md5sum /mnt/games/CustomLibrary/ready-at-dawn-echo-arena/bin/win10/pnsradgameserver.dll
      3. md5sum /mnt/games/CustomLibrary/ready-at-dawn-echo-arena/bin/win10/dbgcore.dll
      4. If exists: md5sum gameserverlegacy.dll, telemetryagent.dll
    Expected Result: All DLLs cataloged with hashes
    Evidence: Hash values saved to .sisyphus/evidence/task-1-dll-hashes.txt

  Scenario: Catalog backup DLLs
    Tool: Bash
    Preconditions: None
    Steps:
      1. ls -la /mnt/games/CustomLibrary/echovr-34.4.631547.1/bin/win10/*.dll
      2. md5sum /mnt/games/CustomLibrary/echovr-34.4.631547.1/bin/win10/pnsradgameserver.dll
      3. md5sum /mnt/games/CustomLibrary/echovr-34.4.631547.1/bin/win10/dbgcore.dll
    Expected Result: Backup DLLs cataloged
    Evidence: Hash values appended to evidence file

  Scenario: Catalog current build DLLs
    Tool: Bash
    Preconditions: None
    Steps:
      1. ls -la /home/andrew/src/nevr-server/dist/nevr-server-v3.2.0+30.83a0518/*.dll
      2. md5sum /home/andrew/src/nevr-server/dist/nevr-server-v3.2.0+30.83a0518/*.dll
    Expected Result: Current build DLLs cataloged
    Evidence: Hash values appended to evidence file
  ```

  **Evidence to Capture:**
  - [ ] `.sisyphus/evidence/task-1-dll-hashes.txt` - All DLL MD5 hashes
  - [ ] `.sisyphus/evidence/task-1-environment-status.md` - Environment summary

  **Commit**: NO

---

- [x] 2. Execute Test A: Backup DLLs (Working Configuration)

  **What to do**:
  - Install ALL backup DLLs to game directory (atomic swap)
  - Remove any extra DLLs not present in backup (e.g., gameserverlegacy.dll)
  - Start game instance via evr-test-harness
  - Trigger player join via HTTP API
  - Capture all logs and events
  - Stop game instance

  **Must NOT do**:
  - Do NOT mix DLLs from different sources
  - Do NOT skip MD5 verification after install

  **Recommended Agent Profile**:
  - **Category**: `unspecified-low`
    - Reason: Straightforward file operations and test execution
  - **Skills**: [`playwright`]
    - `playwright`: For potential browser-based verification if needed

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Sequential
  - **Blocks**: Task 4
  - **Blocked By**: Task 1

  **References**:
  - `/mnt/games/CustomLibrary/echovr-34.4.631547.1/bin/win10/` - Source of backup DLLs
  - `/mnt/games/CustomLibrary/ready-at-dawn-echo-arena/bin/win10/` - Target game directory
  - `/home/andrew/src/evr-test-harness/internal/echovr/session.go` - Session management
  - `/home/andrew/src/evr-test-harness/internal/events/parser.go` - Event parsing patterns

  **Acceptance Criteria**:

  **Agent-Executed QA Scenarios:**

  ```
  Scenario: Install backup DLLs atomically
    Tool: Bash
    Preconditions: Task 1 completed, current DLL state documented
    Steps:
      1. cp /mnt/games/CustomLibrary/echovr-34.4.631547.1/bin/win10/pnsradgameserver.dll \
            /mnt/games/CustomLibrary/ready-at-dawn-echo-arena/bin/win10/
      2. cp /mnt/games/CustomLibrary/echovr-34.4.631547.1/bin/win10/dbgcore.dll \
            /mnt/games/CustomLibrary/ready-at-dawn-echo-arena/bin/win10/
      3. rm -f /mnt/games/CustomLibrary/ready-at-dawn-echo-arena/bin/win10/gameserverlegacy.dll
      4. rm -f /mnt/games/CustomLibrary/ready-at-dawn-echo-arena/bin/win10/gamepatcheslegacy.dll
      5. rm -f /mnt/games/CustomLibrary/ready-at-dawn-echo-arena/bin/win10/telemetryagent.dll
      6. md5sum /mnt/games/CustomLibrary/ready-at-dawn-echo-arena/bin/win10/pnsradgameserver.dll
      7. Assert: Hash matches backup hash from Task 1
      8. ls /mnt/games/CustomLibrary/ready-at-dawn-echo-arena/bin/win10/*.dll | grep -E "gameserverlegacy|gamepatcheslegacy|telemetryagent" | wc -l
      9. Assert: Count is 0 (no NEVR-only DLLs remain)
    Expected Result: Only backup DLLs present (pnsradgameserver.dll, dbgcore.dll), verified by hash
    Evidence: Installation log saved

  Scenario: Start game with backup DLLs
    Tool: evr-mcp tools (via MCP protocol)
    Preconditions: Backup DLLs installed
    Steps:
      1. evr-mcp_echovr_start(http_port=6721, gametype="echo_arena", headless=true, wait_ready=true, timeout_seconds=60)
      2. Assert: session_id returned successfully
      3. evr-mcp_echovr_state(session_id="{session_id}")
      4. Assert: game_status indicates ready state
    Expected Result: Game starts successfully with backup DLLs
    Evidence: Session ID and state captured

  Scenario: Capture events during game running (backup config)
    Tool: Bash + evr-mcp tools
    Preconditions: Game running with backup DLLs
    Steps:
      1. curl -s http://localhost:6721/session (verify game HTTP API is responding)
      2. Sleep 30 seconds to allow ServerDB registration and session initialization
      3. evr-mcp_echovr_events(session_id="{session_id}", types=["error", "state_change", "network"], limit=200)
      4. Copy raw game log: cp /mnt/games/CustomLibrary/ready-at-dawn-echo-arena/_local/r14logs/*.log .sisyphus/evidence/task-2-backup-game.log
    Expected Result: Events captured (expect no errors with backup config)
    Evidence: .sisyphus/evidence/task-2-backup-events.json

  Scenario: Stop game and capture logs
    Tool: evr-mcp tools
    Preconditions: Test complete
    Steps:
      1. evr-mcp_echovr_stop(session_id="{session_id}", force=false, timeout_seconds=30)
      2. Assert: Session stopped successfully
      3. Copy game log file to evidence directory
    Expected Result: Clean shutdown, logs preserved
    Evidence: .sisyphus/evidence/task-2-backup-game.log
  ```

  **Evidence to Capture:**
  - [ ] `.sisyphus/evidence/task-2-backup-events.json` - All events from backup test
  - [ ] `.sisyphus/evidence/task-2-backup-game.log` - Full game log
  - [ ] `.sisyphus/evidence/task-2-backup-state.json` - Game state snapshots

  **Commit**: NO

---

- [x] 3. Execute Test B: Current Build DLLs (Broken Configuration)

  **What to do**:
  - Install ALL current build DLLs to game directory (atomic swap)
  - Include gameserverlegacy.dll, dbgcore.dll from current dist
  - Start game instance via evr-test-harness
  - Trigger player join via HTTP API
  - Capture all logs and events (especially errors!)
  - Stop game instance

  **Must NOT do**:
  - Do NOT mix DLLs from different sources
  - Do NOT skip error event capture

  **Recommended Agent Profile**:
  - **Category**: `unspecified-low`
    - Reason: Same procedure as Task 2, different DLLs
  - **Skills**: [`playwright`]

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Sequential
  - **Blocks**: Task 4
  - **Blocked By**: Task 2

  **References**:
  - `/home/andrew/src/nevr-server/dist/nevr-server-v3.2.0+30.83a0518/` - Source of current DLLs
  - `/mnt/games/CustomLibrary/ready-at-dawn-echo-arena/bin/win10/` - Target game directory

  **Acceptance Criteria**:

  **Agent-Executed QA Scenarios:**

  ```
  Scenario: Install current build DLLs atomically
    Tool: Bash
    Preconditions: Task 2 completed, backup test evidence saved
    Steps:
      1. cp /home/andrew/src/nevr-server/dist/nevr-server-v3.2.0+30.83a0518/pnsradgameserver.dll \
            /mnt/games/CustomLibrary/ready-at-dawn-echo-arena/bin/win10/
      2. cp /home/andrew/src/nevr-server/dist/nevr-server-v3.2.0+30.83a0518/dbgcore.dll \
            /mnt/games/CustomLibrary/ready-at-dawn-echo-arena/bin/win10/
      3. cp /home/andrew/src/nevr-server/dist/nevr-server-v3.2.0+30.83a0518/gameserverlegacy.dll \
            /mnt/games/CustomLibrary/ready-at-dawn-echo-arena/bin/win10/
      4. cp /home/andrew/src/nevr-server/dist/nevr-server-v3.2.0+30.83a0518/gamepatcheslegacy.dll \
            /mnt/games/CustomLibrary/ready-at-dawn-echo-arena/bin/win10/
      5. cp /home/andrew/src/nevr-server/dist/nevr-server-v3.2.0+30.83a0518/telemetryagent.dll \
            /mnt/games/CustomLibrary/ready-at-dawn-echo-arena/bin/win10/
      6. md5sum /mnt/games/CustomLibrary/ready-at-dawn-echo-arena/bin/win10/pnsradgameserver.dll
      7. Assert: Hash matches current build hash from Task 1
    Expected Result: All 5 current build DLLs installed, verified by hash
    Evidence: Installation log saved

  Scenario: Start game with current build DLLs
    Tool: evr-mcp tools
    Preconditions: Current build DLLs installed
    Steps:
      1. evr-mcp_echovr_start(http_port=6721, gametype="echo_arena", headless=true, wait_ready=true, timeout_seconds=60)
      2. Assert: session_id returned (or capture error if fails)
      3. evr-mcp_echovr_state(session_id="{session_id}")
    Expected Result: Game starts (may have issues)
    Evidence: Session ID and state captured

  Scenario: Capture events during game running and capture ERROR events (current build)
    Tool: Bash + evr-mcp tools
    Preconditions: Game running with current build DLLs
    Steps:
      1. curl -s http://localhost:6721/session (verify game HTTP API is responding)
      2. Sleep 30 seconds to allow ServerDB registration and session initialization (error likely occurs here)
      3. evr-mcp_echovr_events(session_id="{session_id}", types=["error", "state_change", "network"], limit=200)
      4. Copy raw game log: cp /mnt/games/CustomLibrary/ready-at-dawn-echo-arena/_local/r14logs/*.log .sisyphus/evidence/task-3-current-game.log
      5. grep -i "error\|fail\|reject\|exception" .sisyphus/evidence/task-3-current-game.log > .sisyphus/evidence/task-3-errors.txt
    Expected Result: Error events captured - THIS IS THE KEY DATA
    Evidence: .sisyphus/evidence/task-3-current-events.json

  Scenario: Stop game and capture logs
    Tool: evr-mcp tools
    Preconditions: Test complete
    Steps:
      1. evr-mcp_echovr_stop(session_id="{session_id}", force=false, timeout_seconds=30)
      2. Copy game log file to evidence directory
    Expected Result: Logs preserved with error details
    Evidence: .sisyphus/evidence/task-3-current-game.log
  ```

  **Evidence to Capture:**
  - [ ] `.sisyphus/evidence/task-3-current-events.json` - All events including ERRORS
  - [ ] `.sisyphus/evidence/task-3-current-game.log` - Full game log with error
  - [ ] `.sisyphus/evidence/task-3-current-state.json` - Game state snapshots

  **Commit**: NO

---

- [x] 4. Compare A/B Test Results and Identify Differences [SKIPPED - Both configs fail to start server mode]

  **What to do**:
  - Diff the event logs from Task 2 and Task 3
  - Diff the game logs from both runs
  - Identify specific error messages in broken config
  - Look for missing events in broken vs working
  - Document sequence of events leading to error
  - Create comparison table

  **Must NOT do**:
  - Do NOT modify raw evidence from Task 2/3 (generating new derived comparison files is allowed)
  - Do NOT assume the cause without evidence

  **Recommended Agent Profile**:
  - **Category**: `unspecified-low`
    - Reason: Text analysis and comparison
  - **Skills**: [`playwright`]

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Sequential
  - **Blocks**: Task 5
  - **Blocked By**: Task 2, 3

  **References**:
  - `.sisyphus/evidence/task-2-*.json` - Backup test results
  - `.sisyphus/evidence/task-3-*.json` - Current test results
  - `/home/andrew/src/nevr-server/src/legacy/gameserver/gameserver.cpp` - Legacy implementation
  - `/home/andrew/src/nevr-server/src/legacy/gameserver/messages.h` - Message symbols

  **Acceptance Criteria**:

  **Agent-Executed QA Scenarios:**

  ```
  Scenario: Diff event logs
    Tool: Bash (diff, jq)
    Preconditions: Both evidence files exist
    Steps:
      1. jq -S '.' .sisyphus/evidence/task-2-backup-events.json > /tmp/backup-sorted.json
      2. jq -S '.' .sisyphus/evidence/task-3-current-events.json > /tmp/current-sorted.json
      3. diff /tmp/backup-sorted.json /tmp/current-sorted.json > .sisyphus/evidence/task-4-events-diff.txt
    Expected Result: Differences documented
    Evidence: .sisyphus/evidence/task-4-events-diff.txt

  Scenario: Extract exact error message from broken config
    Tool: Bash (grep, jq)
    Preconditions: Current events file exists
    Steps:
      1. jq '.[] | select(.type == "error")' .sisyphus/evidence/task-3-current-events.json
      2. grep -i "error\|fail\|reject" .sisyphus/evidence/task-3-current-game.log | head -50
      3. Assert: Error message captured
    Expected Result: Verbatim error message extracted
    Evidence: .sisyphus/evidence/task-4-error-message.txt

  Scenario: Identify missing events in broken config
    Tool: Bash (jq, comm)
    Preconditions: Both event files exist
    Steps:
      1. jq -r '.[].type' .sisyphus/evidence/task-2-backup-events.json | sort | uniq > /tmp/backup-types.txt
      2. jq -r '.[].type' .sisyphus/evidence/task-3-current-events.json | sort | uniq > /tmp/current-types.txt
      3. comm -23 /tmp/backup-types.txt /tmp/current-types.txt (events in backup but not current)
    Expected Result: Missing event types identified
    Evidence: .sisyphus/evidence/task-4-missing-events.txt

  Scenario: Create comparison summary
    Tool: Write tool
    Preconditions: All diffs completed
    Steps:
      1. Create .sisyphus/evidence/task-4-comparison-summary.md with:
         - Error message (verbatim)
         - Sequence of events leading to error
         - Missing events
         - Key differences
    Expected Result: Clear summary document
    Evidence: .sisyphus/evidence/task-4-comparison-summary.md
  ```

  **Evidence to Capture:**
  - [ ] `.sisyphus/evidence/task-4-events-diff.txt` - Event diff
  - [ ] `.sisyphus/evidence/task-4-error-message.txt` - Exact error message
  - [ ] `.sisyphus/evidence/task-4-missing-events.txt` - Events missing in broken
  - [ ] `.sisyphus/evidence/task-4-comparison-summary.md` - Summary document

  **Commit**: NO

---

- [x] 5. Trace Root Cause in Code

  **What to do**:
  - Use the error message from Task 4 to search code
  - Trace the message flow through legacy gameserver
  - Identify which handler is failing or missing
  - Check if WebSocket connection is established
  - Compare with recent commits (cc7acec, 8c4b456, 6d24ea8)
  - Document exact code location of bug

  **Must NOT do**:
  - Do NOT modify any code (read-only investigation)
  - Do NOT assume fix without tracing

  **Recommended Agent Profile**:
  - **Category**: `ultrabrain`
    - Reason: Requires deep code analysis and understanding
  - **Skills**: [`playwright`]

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Sequential
  - **Blocks**: Task 6
  - **Blocked By**: Task 4

  **References**:
  - `/home/andrew/src/nevr-server/src/legacy/gameserver/gameserver.cpp` - Main implementation (lines 276-327 for message handlers)
  - `/home/andrew/src/nevr-server/src/legacy/gameserver/websocket_client.cpp` - WebSocket implementation
  - `/home/andrew/src/nevr-server/src/legacy/gameserver/messages.h` - Symbol definitions
  - `.sisyphus/evidence/task-4-error-message.txt` - Error to trace

  **Acceptance Criteria**:

  **Agent-Executed QA Scenarios:**

  ```
  Scenario: Search code for error message
    Tool: Grep
    Preconditions: Error message known from Task 4
    Steps:
      1. Read error message from task-4-error-message.txt
      2. grep -rn "{error_message}" /home/andrew/src/nevr-server/src/legacy/gameserver/
      3. grep -rn "{error_keywords}" /home/andrew/src/nevr-server/src/
    Expected Result: Code location(s) that produce the error
    Evidence: Code locations documented

  Scenario: Trace message flow for player join
    Tool: Read + AST tools
    Preconditions: None
    Steps:
      1. Read gameserver.cpp, focus on OnTcpMsgLobbyEntrantsV3 (lines 188-195)
      2. Trace SYMBOL_SERVERDB_LOBBY_ENTRANTS_V3 handling (line 287-289)
      3. Check WebSocket message handler (lines 276-327)
      4. Verify broadcaster event forwarding
    Expected Result: Complete message flow documented
    Evidence: Flow diagram in markdown

  Scenario: Compare with working code path
    Tool: Read
    Preconditions: None
    Steps:
      1. Check what backup version does differently (original game's pnsradgameserver.dll)
      2. Note: Backup is original game DLL, not NEVR build - different architecture
      3. Identify what NEVR legacy gameserver is supposed to replicate
    Expected Result: Understand what backup does vs what NEVR attempts
    Evidence: Comparison documented

  Scenario: Document root cause
    Tool: Write
    Preconditions: All investigation complete
    Steps:
      1. Create .sisyphus/evidence/task-5-root-cause.md with:
         - Exact code location of bug
         - Why it fails
         - What the fix should be
         - Files to modify
    Expected Result: Actionable root cause document
    Evidence: .sisyphus/evidence/task-5-root-cause.md
  ```

  **Evidence to Capture:**
  - [ ] `.sisyphus/evidence/task-5-message-flow.md` - Message flow diagram
  - [ ] `.sisyphus/evidence/task-5-root-cause.md` - Root cause analysis

  **Commit**: NO

---

- [x] 6. Document Findings and Create Fix Recommendations

  **What to do**:
  - Compile all evidence into final report
  - Create actionable fix recommendations
  - Propose test case to prevent regression
  - Recommend improvements to evr-test-harness for future testing
  - Save final documentation

  **Must NOT do**:
  - Do NOT implement any fixes (separate plan)
  - Do NOT delete evidence files

  **Recommended Agent Profile**:
  - **Category**: `writing`
    - Reason: Documentation and report writing
  - **Skills**: [`playwright`]

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Sequential (final task)
  - **Blocks**: None
  - **Blocked By**: Task 5

  **References**:
  - All evidence files from Tasks 1-5
  - `/home/andrew/src/nevr-server/.sisyphus/evidence/` - Evidence directory

  **Acceptance Criteria**:

  **Agent-Executed QA Scenarios:**

  ```
  Scenario: Create comprehensive investigation report
    Tool: Write
    Preconditions: All investigation complete
    Steps:
      1. Read all evidence files
      2. Create /home/andrew/src/nevr-server/.sisyphus/evidence/INVESTIGATION-REPORT.md with:
         - Executive summary
         - A/B test results comparison
         - Root cause analysis
         - Fix recommendations (specific code changes)
         - Proposed regression test
         - evr-test-harness improvement suggestions
    Expected Result: Complete, actionable report
    Evidence: INVESTIGATION-REPORT.md

  Scenario: Verify report completeness
    Tool: Bash
    Preconditions: Report written
    Steps:
      1. grep -c "Root Cause" INVESTIGATION-REPORT.md (assert >= 1)
      2. grep -c "Recommendation" INVESTIGATION-REPORT.md (assert >= 1)
      3. grep -c "Test Case" INVESTIGATION-REPORT.md (assert >= 1)
    Expected Result: Report contains all required sections
    Evidence: Validation output
  ```

  **Evidence to Capture:**
  - [ ] `.sisyphus/evidence/INVESTIGATION-REPORT.md` - Final report

  **Commit**: YES (commit evidence and report only)
  - Message: `docs: add player join debugging investigation report`
  - Files: `.sisyphus/evidence/*`
  - Pre-commit: `ls .sisyphus/evidence/*.md | wc -l` (assert >= 1)

---

## Commit Strategy

| After Task | Message | Files | Verification |
|------------|---------|-------|--------------|
| 6 | `docs: add player join debugging investigation report` | `.sisyphus/evidence/*` | Report exists and is complete |

---

## Success Criteria

### Verification Commands
```bash
# All evidence files exist
ls .sisyphus/evidence/task-*.* | wc -l  # Expected: >= 10

# Investigation report exists
test -f .sisyphus/evidence/INVESTIGATION-REPORT.md && echo "PASS"

# Report contains required sections
grep -q "Root Cause" .sisyphus/evidence/INVESTIGATION-REPORT.md && echo "PASS"
grep -q "Recommendation" .sisyphus/evidence/INVESTIGATION-REPORT.md && echo "PASS"
```

### Final Checklist
- [ ] A/B test executed with both DLL configurations
- [ ] Error message from broken config captured verbatim
- [ ] Logs compared and differences documented
- [ ] Root cause identified with exact code location
- [ ] Fix recommendations provided
- [ ] Regression test proposed
