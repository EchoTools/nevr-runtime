# Investigation Report: Echo VR Server Mode Integration Failure

## 1. Executive Summary
The investigation into why Echo VR failed to start in server mode during integration testing has concluded. The primary cause is the **absence of the `-server` command-line flag** during the launch process. 

While a "Hybrid DLL" state (a mix of backup and current build binaries) was initially identified, resolving this by deploying a consistent build did not fix the issue. Static analysis of the `nevr-runtime` source code and the `evr-test-harness` launcher revealed that the game patches explicitly gate dedicated server behavior behind the `-server` flag, which the harness does not currently provide. Consequently, the game consistently boots in client mode, blocking server-specific functionality and HTTP API endpoints.

## 2. Investigation Methodology
The investigation followed a multi-stage approach:
1.  **Environment Audit**: Verified the state of the active game directory, identifying a mix of DLL versions (Task 1).
2.  **Comparative Baseline Testing**: Ran the game with "Backup" DLLs to establish a failure baseline (Task 2).
3.  **Current Build Verification**: Deployed the full "Current Build" (v3.2.0) and repeated tests to isolate DLL versioning as a variable (Task 3).
4.  **Log & API Analysis**: Analyzed `game.log` and HTTP API responses to confirm the game was stuck in client mode (err_code -6).
5.  **Source Code Analysis**:
    *   Analyzed `nevr-runtime/src/gamepatches/patches.cpp` to identify server-mode activation triggers.
    *   Analyzed `evr-test-harness/internal/echovr/process.go` to understand the launch command construction.

## 3. Key Discoveries
*   **Hybrid DLL State (Confirmed)**: The environment was found to be using `pnsradgameserver.dll` from a Jan 7 backup while other DLLs were from the Feb 4 build.
*   **DLL Version Independence**: Both the backup and current build DLLs failed to activate server mode when launched via the harness.
*   **HTTP API Restriction**: The `/session` endpoint consistently returned `{"err_code": -6, "err_str": "Endpoint is restricted in this match type"}`, confirming the game process was in client mode.
*   **Flag Gating Mechanism**: The `gamepatches.dll` (deployed as `dbgcore.dll`) contains a hook that parses the command line. It only sets `isServer = TRUE` and applies dedicated server patches (forcing server flags, allowing incoming connections) if the `-server` flag is present.
*   **Harness Limitation**: The `evr-test-harness` launcher (`evr-mcp_echovr_start`) only passes `-noovr`, `-windowed`, `-httpport`, `-gametype`, and `-mp`. It lacks any logic to include the `-server` flag.

## 4. Root Cause Analysis
The root cause is a **configuration mismatch between the test harness and the game patches**. 

The `nevr-runtime` patches are designed to be non-intrusive; they do not force server behavior unless explicitly requested. This safety mechanism ensures that the same DLLs can be used for both client and server roles. Because the `evr-test-harness` was developed primarily for client-side automation, it does not include the `-server` flag in its process startup logic. Without this flag, the `PatchEnableServer()` function in `dbgcore.dll` is never called, and the game remains a standard client session.

## 5. Fix Recommendations
To resolve the issue and enable server-mode testing, the following steps are required:

1.  **Update Launch Command**: Modify the Echo VR launch sequence to include the `-server` flag.
2.  **Standardize Deployment**: Ensure all 5 NEVR DLLs are deployed from the same build (Current Build v3.2.0 recommended).
3.  **Recommended Server Flags**: For stable server operation, include the following flags:
    *   `-server` (Required)
    *   `-headless` (Recommended for CI/CD environments)
    *   `-noconsole` (Recommended when using `-headless`)

## 6. Proposed Regression Test
To verify the fix:
1.  **Manual Launch**: Run the game via Wine with the `-server` flag:
    ```bash
    wine echovr.exe -noovr -windowed -mp -httpport 6721 -server -level mpl_arena_a
    ```
2.  **Log Verification**: Check `game.log` for the presence of server-mode initialization logs from `nevr-runtime`.
3.  **API Verification**: Query the HTTP API:
    ```bash
    curl http://127.0.0.1:6721/session
    ```
    Verify that the response contains valid session data instead of error code -6.

## 7. evr-test-harness Improvements
The `evr-test-harness` should be extended to support server-mode orchestration:
*   **New Parameter**: Add a `server_mode` boolean to the `evr-mcp_echovr_start` JSON input.
*   **Logic Update**: In `internal/echovr/process.go`, append `-server` to the `args` slice if `server_mode` is true.
*   **Config Support**: Allow passing a `-config-path` to specify custom `serverdb_host` settings for the `gameserver.dll` component.
