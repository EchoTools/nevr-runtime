# Phase 2 & 3: Directory Reorganization Plan

## TL;DR

> **Quick Summary**: Reorganize C++ CMake project from flat structure into src/, tools/, admin/ hierarchy. Fix dbghooks build issue, update CMake path references, handle legacy component relative includes, and optionally consolidate duplicate code.
> 
> **Deliverables**:
> - Hierarchical directory structure (src/, tools/, admin/)
> - Fixed dbghooks CMake integration
> - Updated CMake path references (5 files)
> - Fixed legacy component includes (9 C++ files, 15 include statements)
> - Clean build verification after each commit
> 
> **Estimated Effort**: Medium (3-5 hours with verification)
> **Parallel Execution**: NO - Sequential with verification checkpoints
> **Critical Path**: Fix dbghooks → Move directories → Update CMake → Fix includes → Verify

---

## Context

### Original Request
User requested detailed execution plan for Phase 2 and Phase 3 of a directory reorganization for the nevr-runtime C++ CMake project after Phase 1 (lowercase renames) was completed.

### Interview Summary
**Key Discussions**:
- **Risk tolerance**: Conservative - stop immediately if build breaks, analyze before proceeding
- **Commit strategy**: Logical grouping - 3-5 commits, each must pass full verification
- **Legacy common/ decision**: Based on analysis - keep separate (they're distinct, not duplicates)

**Research Findings**:
- **dbghooks status**: Directory exists with binaries but NO CMakeLists.txt; add_subdirectory(dbghooks) on line 153 of root CMakeLists.txt will fail
- **Legacy common/ analysis**: NOT duplicates - legacy dirs contain game structure definitions, root common/ contains utility code. Keep separate.
- **CMake dependencies**: 5 CMakeLists.txt files need path updates across ~20 lines
- **Fragile includes**: 9 C++ files with 15 relative include statements will break when moving from depth-2 to depth-4

### Metis Review
**Research-Based Decisions** (no additional gaps identified):
- dbghooks: Comment out add_subdirectory() line, move to tools/, document as development tool
- Legacy common/: Keep separate - they serve different purposes (confirmed via file comparison)
- Path update timing: Update CMake paths after directory moves in same commit (atomic)
- Legacy includes: Fix ../../ paths after moving to src/legacy/ using automated sed script (atomic with move)

---

## Work Objectives

### Core Objective
Reorganize nevr-runtime C++ project from flat directory structure into hierarchical src/, tools/, admin/ layout while maintaining 100% build compatibility and git history.

### Concrete Deliverables
- `src/` directory containing all source components (common, gamepatches, gameserver, telemetryagent, protobufnevr, legacy/gamepatches, legacy/gameserver)
- `tools/` directory containing development utilities (dbghooks)
- `admin/` directory containing operational tools (supervisor)
- Updated CMakeLists.txt files with correct paths
- Fixed C++ include statements in legacy components
- Verified working build (make configure && make build && make dist)

### Definition of Done
- [ ] All source components moved to src/
- [ ] All development tools moved to tools/
- [ ] All admin tools moved to admin/
- [ ] Root CMakeLists.txt references updated (dbghooks commented out)
- [ ] 5 CMakeLists.txt files updated with correct paths
- [ ] 9 C++ files updated with corrected relative includes
- [ ] `make clean && make configure` succeeds
- [ ] `make build` succeeds with zero errors
- [ ] `make dist` succeeds and creates distribution packages
- [ ] Git history preserved (all moves use `git mv`)

### Must Have
- Working build after EVERY commit
- Git history preserved (use `git mv`)
- Conservative approach (stop on any failure)
- Clear commit messages explaining changes

### Must NOT Have (Guardrails)
- Broken intermediate states (each commit must build)
- Direct file copying (must use `git mv`)
- Changes to external dependencies (extern/ stays untouched)
- Modifications to git submodules (nevr-common)
- Manual file path edits without verification
- Skipping verification steps

---

## Verification Strategy

### Automated Verification Only (NO User Intervention)

Each TODO includes EXECUTABLE verification procedures that can be run directly:

**Build Verification (After Every Commit)**:
```bash
# Full build verification sequence
make clean
make configure  # CMake must succeed
make build      # Compilation must succeed
make dist       # Distribution packaging must succeed
```

**Evidence Requirements**:
- Exit code 0 from all make commands
- No "error:" messages in output
- Distribution packages created in dist/ directory
- Terminal output captured for commit documentation

**By Deliverable Type**:

| Type | Verification Tool | Automated Procedure |
|------|------------------|---------------------|
| **Directory Structure** | ls, find via Bash | Verify directories exist, list contents, confirm structure |
| **Git History** | git log via Bash | Verify commits use git mv (R100 rename detection) |
| **CMake Configuration** | make configure via Bash | Parse output for SUCCESS, check for errors |
| **Build Success** | make build via Bash | Check exit code, count compiled objects |
| **Distribution** | make dist via Bash | Verify package files created, check sizes |

---

## Execution Strategy

### Sequential Execution (No Parallelization)

> Each step MUST complete successfully before proceeding to the next.
> Conservative approach: stop immediately on any failure.

```
Step 1: Pre-Flight Checks
├── Verify current build works
├── Document baseline state
└── Create backup branch

Step 2: Fix dbghooks CMake Issue
├── Comment out add_subdirectory(dbghooks)
├── Remove dependent references
└── VERIFY: make clean && make configure && make build && make dist

Step 3: Create Directory Structure & Move Components (Atomic)
├── Create src/, tools/, admin/ directories
├── Execute git mv for all components
├── Update all CMakeLists.txt path references
├── Fix all legacy component includes
└── VERIFY: make clean && make configure && make build && make dist

Step 4: Final Validation
├── Run full build verification
├── Check distribution packages
├── Verify git history preservation
└── Update documentation

Critical Path: Step 1 → Step 2 → Step 3 → Step 4
No parallel execution due to sequential dependencies
```

### Dependency Matrix

| Step | Depends On | Blocks | Parallel With |
|------|------------|--------|---------------|
| 1. Pre-flight | None | 2, 3, 4 | None |
| 2. Fix dbghooks | 1 | 3, 4 | None |
| 3. Move & Update | 1, 2 | 4 | None |
| 4. Final Validation | 1, 2, 3 | None | None |

---

## TODOs

- [ ] 1. Pre-Flight Verification & Baseline

  **What to do**:
  - Verify current build works on HEAD (4519bb8)
  - Document current directory structure
  - Create backup branch for safety
  - Capture baseline build output

  **Must NOT do**:
  - Skip baseline verification
  - Proceed if current build is broken
  - Assume build works without testing

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: Simple verification task, no code changes
  - **Skills**: None required
    - This is a straightforward bash script execution task

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Sequential
  - **Blocks**: Steps 2, 3, 4 (everything depends on this)
  - **Blocked By**: None (can start immediately)

  **References**:

  **Build System**:
  - `/home/andrew/src/nevr-server/CMakeLists.txt` - Root build configuration
  - `/home/andrew/src/nevr-server/scripts/build-with-wine.sh` - Build script reference
  - `/home/andrew/src/nevr-server/Makefile` - Make targets (clean, configure, build, dist)

  **Documentation**:
  - `README.md` - Project overview and build instructions
  - `.github/copilot-instructions.md` - Build workflow guidance

  **Acceptance Criteria**:

  **Baseline Build Verification**:
  ```bash
  # Execute full build verification
  cd /home/andrew/src/nevr-server
  git status  # Verify clean working tree or document uncommitted changes
  git log -1 --oneline  # Confirm HEAD is 4519bb8
  make clean
  make configure
  # Assert: Exit code 0, "-- Build files have been written" in output
  make build
  # Assert: Exit code 0, no "error:" strings in output
  make dist
  # Assert: Exit code 0, dist/ directory created with packages
  ```

  **Directory Structure Documentation**:
  ```bash
  # Capture current structure
  ls -1 /home/andrew/src/nevr-server/ | grep -v '^\.' > /tmp/structure-before.txt
  # Assert: File contains: common, gamepatches, gameserver, etc. (flat structure)
  ```

  **Backup Branch Creation**:
  ```bash
  git branch backup-before-reorganization
  git branch --list backup-before-reorganization
  # Assert: Branch exists in output
  ```

  **Evidence to Capture**:
  - [ ] Terminal output from `make configure` → /tmp/baseline-configure.log
  - [ ] Terminal output from `make build` → /tmp/baseline-build.log
  - [ ] Terminal output from `make dist` → /tmp/baseline-dist.log
  - [ ] Current directory listing → /tmp/structure-before.txt
  - [ ] Git status showing branch name and HEAD commit

  **Commit**: NO (verification only, no changes)

---

- [ ] 2. Fix dbghooks CMake Issue

  **What to do**:
  - Comment out `add_subdirectory(dbghooks)` on line 153 of root CMakeLists.txt
  - Add explanatory comment about why it's disabled
  - Comment out or remove `DEPENDS dbghooks` from dist targets (lines 233, 309)
  - Keep file copy commands for dbghooks config files (will update paths later)
  - Verify build succeeds without dbghooks

  **Must NOT do**:
  - Create a CMakeLists.txt for dbghooks (not in scope)
  - Remove dbghooks directory
  - Delete file copy commands for config files
  - Merge feature branch (out of scope)

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: Simple file edit with clear requirements
  - **Skills**: None required
    - Standard edit operation on CMakeLists.txt

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Sequential
  - **Blocks**: Step 3 (move operations)
  - **Blocked By**: Step 1 (baseline verification)

  **References**:

  **File to Edit**:
  - `/home/andrew/src/nevr-server/CMakeLists.txt:153` - add_subdirectory(dbghooks) line to comment out
  - `/home/andrew/src/nevr-server/CMakeLists.txt:233` - DEPENDS dbghooks in dist-prepare target
  - `/home/andrew/src/nevr-server/CMakeLists.txt:309` - DEPENDS dbghooks in dist-lite-prepare target

  **Research Context**:
  - dbghooks/ exists but has NO CMakeLists.txt (contains only binaries and Python venv)
  - Referenced in 11 places in root CMakeLists.txt
  - Will be moved to tools/dbghooks/ in next step

  **Acceptance Criteria**:

  **Edit Root CMakeLists.txt**:
  ```bash
  # Comment out line 153 with explanation
  cd /home/andrew/src/nevr-server
  
  # Verify current line content
  sed -n '153p' CMakeLists.txt
  # Assert: Line contains "add_subdirectory(dbghooks)"
  
  # Replace line 153 with commented version
  sed -i '153s/^add_subdirectory(dbghooks)$/# add_subdirectory(dbghooks)  # TODO: dbghooks has no CMakeLists.txt - will move to tools\/dbghooks\//' CMakeLists.txt
  
  # Verify change
  sed -n '152,154p' CMakeLists.txt
  # Assert: Line 153 now starts with "# add_subdirectory(dbghooks)"
  ```

  **Remove dbghooks Dependency**:
  ```bash
  # Find and comment out DEPENDS dbghooks lines
  grep -n "DEPENDS.*dbghooks" CMakeLists.txt
  # Assert: Shows lines 233 and 309
  
  # Comment out line 233
  sed -i '233s/^  DEPENDS/  # DEPENDS/' CMakeLists.txt
  
  # Comment out line 309
  sed -i '309s/^  DEPENDS/  # DEPENDS/' CMakeLists.txt
  
  # Verify changes
  sed -n '233p;309p' CMakeLists.txt
  # Assert: Both lines now start with "#"
  ```

  **Build Verification**:
  ```bash
  make clean
  make configure
  # Assert: Exit code 0, NO error about missing dbghooks/CMakeLists.txt
  # Assert: Output does NOT contain "CMakeLists.txt file" error
  
  make build
  # Assert: Exit code 0, build succeeds
  
  make dist
  # Assert: Exit code 0, distribution created (may lack dbghooks.dll but that's expected)
  ```

  **Evidence to Capture**:
  - [ ] Terminal output from sed commands showing changes
  - [ ] Output from `make configure` showing NO dbghooks error
  - [ ] Output from `make build` showing success
  - [ ] Output from `make dist` showing success

  **Commit**: YES
  - Message: `build: comment out dbghooks add_subdirectory (no CMakeLists.txt exists)`
  - Files: `CMakeLists.txt`
  - Pre-commit: `make configure && make build`

---

- [ ] 3. Move Directory Structure & Update Paths (Atomic)

  **What to do**:
  - Create src/, tools/, admin/ directories
  - Use `git mv` to move all components preserving history:
    - common, gamepatches, gameserver, telemetryagent, protobufnevr → src/
    - gamepatcheslegacy, gameserverlegacy → src/ (will create legacy subdir)
    - dbghooks → tools/
    - supervisor → admin/
  - Create src/legacy/ and move legacy components into it
  - Update 5 CMakeLists.txt files with corrected paths
  - Fix 15 relative includes in 9 C++ legacy files
  - All changes in ONE atomic commit

  **Must NOT do**:
  - Use `mv` instead of `git mv` (breaks history)
  - Move extern/ or cmake/ directories (they stay at root)
  - Modify external submodule files (nevr-common)
  - Split into multiple commits (must be atomic to avoid broken state)
  - Skip any CMakeLists.txt updates

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: Complex multi-file refactoring with many interdependencies
  - **Skills**: None required
    - Core bash operations (git mv, sed, edit)

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Sequential
  - **Blocks**: Step 4 (final validation)
  - **Blocked By**: Steps 1, 2 (baseline and dbghooks fix)

  **References**:

  **CMakeLists.txt Files to Update** (with specific line numbers):
  - `/home/andrew/src/nevr-server/CMakeLists.txt:139` - include_directories: `common` → `src/common`
  - `/home/andrew/src/nevr-server/CMakeLists.txt:151-158` - add_subdirectory calls: add `src/`, `tools/`, `admin/` prefixes
  - `/home/andrew/src/nevr-server/CMakeLists.txt:227,229,303,305` - dbghooks paths: `dbghooks/` → `tools/dbghooks/`
  - `/home/andrew/src/nevr-server/common/CMakeLists.txt:13` - include_directories: `${CMAKE_SOURCE_DIR}/common` → `${CMAKE_SOURCE_DIR}/src/common`
  - `/home/andrew/src/nevr-server/gameserver/CMakeLists.txt:23` - include_directories: `${CMAKE_SOURCE_DIR}/common` → `${CMAKE_SOURCE_DIR}/src/common`
  - `/home/andrew/src/nevr-server/telemetryagent/CMakeLists.txt:48` - include_directories: `${CMAKE_SOURCE_DIR}/common` → `${CMAKE_SOURCE_DIR}/src/common`
  - `/home/andrew/src/nevr-server/protobufnevr/CMakeLists.txt:5` - relative path: `../extern/nevr-common` → `../../extern/nevr-common`

  **C++ Files with Fragile Includes** (9 files, 15 include statements):
  - `gamepatcheslegacy/common/echovr.h:7,8,9` - Three `../../` includes
  - `gamepatcheslegacy/common/echovr_internal.cpp:5` - One `../../` include
  - `gamepatcheslegacy/common/echovr_unexported.h:4` - One `../../` include
  - `gamepatcheslegacy/common/pch.h:17,22` - Two `../../` includes
  - `gameserverlegacy/common/echovr.h:10,11,12` - Three `../../` includes
  - `gameserverlegacy/common/echovr_internal.cpp:5` - One `../../` include
  - `gameserverlegacy/common/echovr_unexported.h:4` - One `../../` include
  - `gameserverlegacy/common/pch.h:17` - One `../../` include

  **Include Path Mappings**:
  After move from `gamepatcheslegacy/common/` → `src/legacy/gamepatches/common/` (depth 2→4):
  - `../../common/` → `../../../common/` (resolve to src/common after move)
  - `../../extern/` → `../../../../extern/`
  - `../../winsock2.h` → `../../../../winsock2.h`

  **Acceptance Criteria**:

  **Part A: Create Directory Structure**:
  ```bash
  cd /home/andrew/src/nevr-server
  
  # Create top-level directories
  mkdir -p src/legacy tools admin
  
  # Verify creation
  ls -ld src/ tools/ admin/
  # Assert: All three directories exist
  ```

  **Part B: Move Components with git mv**:
  ```bash
  # Move main source components to src/
  git mv common src/
  git mv gamepatches src/
  git mv gameserver src/
  git mv telemetryagent src/
  git mv protobufnevr src/
  
  # Move legacy components to src/ temporarily
  git mv gamepatcheslegacy src/
  git mv gameserverlegacy src/
  
  # Reorganize legacy components into src/legacy/
  mkdir -p src/legacy
  git mv src/gamepatcheslegacy src/legacy/gamepatches
  git mv src/gameserverlegacy src/legacy/gameserver
  
  # Move tools and admin
  git mv dbghooks tools/
  git mv supervisor admin/
  
  # Verify moves
  git status --short
  # Assert: Should show "R100" (rename with 100% similarity) for all moves
  
  ls -1 src/
  # Assert: Shows common, gamepatches, gameserver, legacy, protobufnevr, telemetryagent
  
  ls -1 src/legacy/
  # Assert: Shows gamepatches, gameserver
  
  ls -1 tools/
  # Assert: Shows dbghooks
  
  ls -1 admin/
  # Assert: Shows supervisor
  ```

  **Part C: Update Root CMakeLists.txt**:
  ```bash
  cd /home/andrew/src/nevr-server
  
  # Line 139: Update include_directories
  sed -i '139s|"${CMAKE_SOURCE_DIR}/common"|"${CMAKE_SOURCE_DIR}/src/common"|' CMakeLists.txt
  
  # Lines 151-158: Update add_subdirectory calls
  sed -i '151s|add_subdirectory(common)|add_subdirectory(src/common)|' CMakeLists.txt
  sed -i '152s|add_subdirectory(protobufnevr)|add_subdirectory(src/protobufnevr)|' CMakeLists.txt
  # Note: Line 153 already commented out (dbghooks)
  sed -i '154s|add_subdirectory(gamepatches)|add_subdirectory(src/gamepatches)|' CMakeLists.txt
  sed -i '155s|add_subdirectory(gamepatcheslegacy)|add_subdirectory(src/legacy/gamepatches)|' CMakeLists.txt
  sed -i '156s|add_subdirectory(gameserver)|add_subdirectory(src/gameserver)|' CMakeLists.txt
  sed -i '157s|add_subdirectory(gameserverlegacy)|add_subdirectory(src/legacy/gameserver)|' CMakeLists.txt
  sed -i '158s|add_subdirectory(telemetryagent)|add_subdirectory(src/telemetryagent)|' CMakeLists.txt
  sed -i '159s|add_subdirectory(supervisor)|add_subdirectory(admin/supervisor)|' CMakeLists.txt
  
  # Lines 227, 229, 303, 305: Update dbghooks paths
  sed -i 's|${CMAKE_SOURCE_DIR}/dbghooks/|${CMAKE_SOURCE_DIR}/tools/dbghooks/|g' CMakeLists.txt
  
  # Verify changes
  sed -n '139p;151p;152p;154p;155p;156p;157p;158p;159p' CMakeLists.txt
  # Assert: All paths now have src/, tools/, or admin/ prefixes
  
  grep "tools/dbghooks" CMakeLists.txt
  # Assert: Shows updated paths
  ```

  **Part D: Update Component CMakeLists.txt Files**:
  ```bash
  # Update src/common/CMakeLists.txt line 13
  sed -i '13s|${CMAKE_SOURCE_DIR}/common|${CMAKE_SOURCE_DIR}/src/common|' src/common/CMakeLists.txt
  
  # Update src/gameserver/CMakeLists.txt line 23
  sed -i '23s|"${CMAKE_SOURCE_DIR}/common"|"${CMAKE_SOURCE_DIR}/src/common"|' src/gameserver/CMakeLists.txt
  
  # Update src/telemetryagent/CMakeLists.txt line 48
  sed -i '48s|"${CMAKE_SOURCE_DIR}/common"|"${CMAKE_SOURCE_DIR}/src/common"|' src/telemetryagent/CMakeLists.txt
  
  # Update src/protobufnevr/CMakeLists.txt line 5 (relative path depth change)
  sed -i '5s|../extern/nevr-common|../../extern/nevr-common|' src/protobufnevr/CMakeLists.txt
  
  # Verify all changes
  grep "src/common" src/common/CMakeLists.txt src/gameserver/CMakeLists.txt src/telemetryagent/CMakeLists.txt
  # Assert: All show ${CMAKE_SOURCE_DIR}/src/common
  
  sed -n '5p' src/protobufnevr/CMakeLists.txt
  # Assert: Shows ../../extern/nevr-common
  ```

  **Part E: Fix Legacy Component Includes**:
  ```bash
  # Fix gamepatches legacy includes (depth 2→4: ../../ → ../../../ or ../../../../)
  
  # Pattern 1: ../../common/ → ../../../common/ (resolves to src/common after move)
  sed -i 's|#include "../../common/|#include "../../../common/|g' \
    src/legacy/gamepatches/common/echovr.h \
    src/legacy/gamepatches/common/echovr_internal.cpp
  
  # Pattern 2: ../../extern/ → ../../../../extern/
  sed -i 's|#include "../../extern/|#include "../../../../extern/|g' \
    src/legacy/gamepatches/common/echovr.h \
    src/legacy/gamepatches/common/echovr_unexported.h \
    src/legacy/gamepatches/common/pch.h
  
  # Pattern 3: ../../winsock2.h → ../../../../winsock2.h
  sed -i 's|#include "../../winsock2.h"|#include "../../../../winsock2.h"|g' \
    src/legacy/gamepatches/common/pch.h
  
  # Fix gameserver legacy includes (same patterns)
  sed -i 's|#include "../../common/|#include "../../../common/|g' \
    src/legacy/gameserver/common/echovr.h \
    src/legacy/gameserver/common/echovr_internal.cpp
  
  sed -i 's|#include "../../extern/|#include "../../../../extern/|g' \
    src/legacy/gameserver/common/echovr.h \
    src/legacy/gameserver/common/echovr_unexported.h
  
  sed -i 's|#include "../../winsock2.h"|#include "../../../../winsock2.h"|g' \
    src/legacy/gameserver/common/pch.h
  
  # Verify all changes
  grep -n '#include "\.\.\/' src/legacy/gamepatches/common/*.{h,cpp} src/legacy/gameserver/common/*.{h,cpp} | head -20
  # Assert: All includes now show ../../../ or ../../../../ (no ../../)
  ```

  **Part F: Build Verification**:
  ```bash
  make clean
  make configure
  # Assert: Exit code 0
  # Assert: Output contains "-- Build files have been written"
  # Assert: NO errors about missing directories or files
  
  make build
  # Assert: Exit code 0
  # Assert: All targets compile successfully
  # Assert: NO "fatal error: file not found" messages
  
  make dist
  # Assert: Exit code 0
  # Assert: Distribution packages created in dist/
  
  # Verify dist contents
  ls -lh dist/
  # Assert: DLL files present, similar sizes to baseline
  ```

  **Evidence to Capture**:
  - [ ] `git status --short` output showing R100 renames
  - [ ] Directory structure after moves: `find src/ tools/ admin/ -type d`
  - [ ] Terminal output from all sed commands
  - [ ] Output from `make configure` showing success
  - [ ] Output from `make build` showing all targets compiled
  - [ ] Output from `make dist` showing packages created
  - [ ] File listing of dist/ directory

  **Commit**: YES
  - Message: `refactor: reorganize directory structure into src/, tools/, admin/ hierarchy`
  - Files: All moved directories, 5 CMakeLists.txt files, 9 C++ files
  - Pre-commit: `make configure && make build && make dist`

---

- [ ] 4. Final Validation & Documentation

  **What to do**:
  - Run complete build verification from clean state
  - Compare dist/ output with baseline
  - Verify git history shows proper renames (R100)
  - Document new directory structure in commit message addendum
  - Create final validation report

  **Must NOT do**:
  - Skip any verification steps
  - Assume everything works without testing
  - Proceed to Phase 3 without confirming Phase 2 success

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: Verification and documentation task
  - **Skills**: None required
    - Standard bash verification commands

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Sequential
  - **Blocks**: None (final step)
  - **Blocked By**: Steps 1, 2, 3 (depends on all previous steps)

  **References**:

  **Baseline Files** (from Step 1):
  - `/tmp/baseline-configure.log` - Original CMake configure output
  - `/tmp/baseline-build.log` - Original build output
  - `/tmp/baseline-dist.log` - Original dist output
  - `/tmp/structure-before.txt` - Original directory structure

  **Acceptance Criteria**:

  **Clean Build Verification**:
  ```bash
  cd /home/andrew/src/nevr-server
  
  # Start from absolutely clean state
  make clean
  rm -rf build/
  
  # Full build sequence
  make configure > /tmp/final-configure.log 2>&1
  # Assert: Exit code 0
  
  make build > /tmp/final-build.log 2>&1
  # Assert: Exit code 0
  
  make dist > /tmp/final-dist.log 2>&1
  # Assert: Exit code 0
  
  # Verify no errors in logs
  grep -i "error" /tmp/final-configure.log /tmp/final-build.log /tmp/final-dist.log
  # Assert: Exit code 1 (no matches) or only benign warnings
  ```

  **Distribution Comparison**:
  ```bash
  # Compare baseline dist/ with current dist/
  ls -lh dist/ > /tmp/dist-after.txt
  
  # Check that expected DLLs are present
  ls dist/*.dll
  # Assert: gamepatches.dll, gameserver.dll, telemetryagent.dll present
  
  # Compare file sizes (should be similar ±10%)
  stat -c "%s %n" dist/*.dll
  # Manual review: Sizes should match baseline roughly
  ```

  **Git History Verification**:
  ```bash
  # Verify git mv preserved history (100% similarity)
  git log --follow --stat --oneline -- src/common/ | head -20
  # Assert: Shows history including commits from when it was just "common/"
  
  git log --stat --oneline -1
  # Assert: Shows renamed files with R100 (100% similarity)
  
  # Verify no unintended changes
  git diff HEAD~1 -- src/common/base64.cpp
  # Assert: Only shows path changes, no content changes
  ```

  **Directory Structure Documentation**:
  ```bash
  # Document final structure
  find src/ tools/ admin/ -type d | sort > /tmp/structure-after.txt
  
  # Create structure comparison
  echo "=== BEFORE ===" > /tmp/structure-comparison.txt
  cat /tmp/structure-before.txt >> /tmp/structure-comparison.txt
  echo -e "\n=== AFTER ===" >> /tmp/structure-comparison.txt
  cat /tmp/structure-after.txt >> /tmp/structure-comparison.txt
  
  cat /tmp/structure-comparison.txt
  # Assert: Shows flat structure transformed to hierarchical
  ```

  **Final Validation Report**:
  ```bash
  # Generate validation report
  cat > /tmp/phase2-validation-report.txt <<'EOF'
# Phase 2 Validation Report

## Build Status
- CMake Configure: PASS
- Build: PASS
- Distribution: PASS

## Structure Changes
- Created: src/, tools/, admin/
- Moved: 10 components to new hierarchy
- Updated: 5 CMakeLists.txt files
- Fixed: 9 C++ files with relative includes

## Git History
- All moves used git mv
- History preserved (R100 detection)
- Commits: 2 total (dbghooks fix + reorganization)

## Verification
- make configure: SUCCESS
- make build: SUCCESS
- make dist: SUCCESS
- Distribution packages created
- No build errors or warnings

## Ready for Phase 3
Phase 2 complete. Phase 3 (legacy common consolidation) was evaluated during research.
Decision: Keep legacy common/ directories separate (they serve different purposes).
No Phase 3 work needed.
EOF
  
  cat /tmp/phase2-validation-report.txt
  ```

  **Evidence to Capture**:
  - [ ] Final build logs (configure, build, dist)
  - [ ] Distribution file listing and sizes
  - [ ] Git log showing preserved history
  - [ ] Structure comparison (before/after)
  - [ ] Phase 2 validation report

  **Commit**: NO (validation only, documentation already in previous commits)

---

## Phase 3 Assessment: Legacy Common Consolidation

### Research Findings

During investigation, comprehensive analysis revealed:

**Legacy common/ directories are NOT duplicates:**
- `gamepatcheslegacy/common/` and `gameserverlegacy/common/` contain **game structure definitions** (echovr.h, echovr_internal.h)
- Root `common/` contains **utility code** (logging, base64, globals, symbols)
- They serve **different purposes** and are complementary, not duplicative

**Specific differences:**
- `echovr_internal.cpp` is identical in both legacy dirs (could be consolidated within legacy)
- `echovr.h` differs: gameserver version has 163 more lines of documentation
- `pch.h` has minor differences: shellapi.h include path

**Build architecture is correct:**
- Legacy components are intentionally self-contained
- Active components use `Common` static library from root
- Only shared dependency is `platform_stubs.h` (LSP stub file)

### Decision: NO Phase 3 Work Required

**Rationale:**
1. Legacy common/ and root common/ serve different purposes
2. No actual code duplication exists
3. Build architecture is already correct
4. Consolidation would break "frozen v1" isolation guarantee
5. Optional micro-optimization (consolidating echovr_internal.cpp within legacy) has minimal benefit

**Recommendation**: Document this decision and close Phase 3 as "not applicable".

---

## Commit Strategy

| After Task | Message | Files | Verification |
|------------|---------|-------|--------------|
| 2 | `build: comment out dbghooks add_subdirectory (no CMakeLists.txt exists)` | CMakeLists.txt | make configure && make build |
| 3 | `refactor: reorganize directory structure into src/, tools/, admin/ hierarchy` | All moved dirs, 5 CMakeLists.txt, 9 C++ files | make configure && make build && make dist |

---

## Success Criteria

### Verification Commands
```bash
# From clean state
make clean
make configure  # Expected: Exit 0, "Build files written"
make build      # Expected: Exit 0, all targets compiled
make dist       # Expected: Exit 0, packages in dist/
```

### Final Checklist
- [ ] All "Must Have" present:
  - [ ] Working build after every commit
  - [ ] Git history preserved (git mv used)
  - [ ] Conservative approach (stopped on any failure)
  - [ ] Clear commit messages
- [ ] All "Must NOT Have" absent:
  - [ ] No broken intermediate states
  - [ ] No direct file copying
  - [ ] No changes to extern/ or nevr-common submodule
  - [ ] All verification steps completed
- [ ] All deliverables complete:
  - [ ] src/, tools/, admin/ directory structure
  - [ ] Updated CMakeLists.txt files
  - [ ] Fixed C++ includes
  - [ ] Verified working build
  - [ ] Distribution packages created
- [ ] Phase 3 assessment documented (no work needed)

---

## Risk Mitigation

### If Step 2 (dbghooks fix) Fails

**Symptom**: Build fails even after commenting out add_subdirectory(dbghooks)

**Diagnosis**:
```bash
make configure 2>&1 | grep -A5 -B5 dbghooks
# Look for any remaining references causing issues
```

**Mitigation**:
1. Check if there are other add_subdirectory(dbghooks) lines
2. Verify DEPENDS dbghooks was properly commented out
3. Check for any target_link_libraries(... dbghooks ...) references
4. If found, comment out those as well

**Rollback**:
```bash
git checkout CMakeLists.txt
# Return to baseline, reassess problem
```

---

### If Step 3 (move & update) Fails During Configure

**Symptom**: `make configure` fails with "CMakeLists.txt not found" or similar

**Diagnosis**:
```bash
# Check what CMake is looking for
make configure 2>&1 | grep "CMakeLists.txt"
# Check if directories were moved correctly
ls -la src/ tools/ admin/
```

**Mitigation**:
1. Verify all git mv commands completed successfully
2. Check for typos in add_subdirectory() paths
3. Verify src/legacy/ subdirectory was created
4. Check that extern/ is still at root (not moved)

**Rollback**:
```bash
git reset --hard HEAD
# Undo all changes in Step 3, return to post-Step-2 state
# Re-execute Step 3 with corrections
```

---

### If Step 3 Fails During Build (Include Errors)

**Symptom**: `fatal error: platform_stubs.h: No such file or directory`

**Diagnosis**:
```bash
# Find which file is failing
make build 2>&1 | grep "fatal error"
# Check the problematic include statement
grep -n "platform_stubs.h" src/legacy/*/common/*.{h,cpp}
```

**Mitigation**:
1. Verify all sed commands for include fixes ran successfully
2. Check that `../../../` was used correctly (not `../../`)
3. Manually inspect and fix any missed includes:
   ```bash
   # For files in src/legacy/*/common/ needing src/common/platform_stubs.h:
   # Path should be: ../../../common/ (up 3 to src/, then into common/)
   ```

**Rollback**:
```bash
git reset --hard HEAD
# Undo all changes, return to post-Step-2 state
# Re-execute Step 3 with corrected sed patterns
```

---

### If Step 3 Fails During Dist (File Copy Errors)

**Symptom**: `Error copying file: dbghooks/gun2cr_config.ini not found`

**Diagnosis**:
```bash
# Check if config files exist
ls -la tools/dbghooks/gun2cr_config.ini
ls -la tools/dbghooks/gun2cr_fix/weapon_config.json
```

**Mitigation**:
1. If files don't exist: They may not be in current branch
2. Check if paths in CMakeLists.txt were updated to tools/dbghooks/
3. If files genuinely missing, remove those copy commands:
   ```bash
   # Comment out file copy commands that reference missing files
   sed -i '/gun2cr_config.ini/s/^  COMMAND/#  COMMAND/' CMakeLists.txt
   sed -i '/weapon_config.json/s/^  COMMAND/#  COMMAND/' CMakeLists.txt
   ```

**Rollback**: Not needed if mitigation works (commenting out is safe)

---

### General Rollback Strategy

**If any step fails catastrophically:**

```bash
# Nuclear option: Return to backup branch
git checkout backup-before-reorganization

# Then debug the issue on a test branch
git checkout -b test-reorganization
# Retry steps with fixes
```

**Conservative checkpoints:**
```bash
# After each successful step, tag it
git tag -a phase2-step2-complete -m "dbghooks fix complete"
git tag -a phase2-step3-complete -m "reorganization complete"

# If needed, return to checkpoint
git reset --hard phase2-step2-complete
```

---

## Estimated Time & Complexity

### Time Breakdown

| Task | Estimated Time | Risk Level |
|------|----------------|------------|
| 1. Pre-flight verification | 10-15 min | LOW |
| 2. Fix dbghooks | 10-15 min | LOW |
| 3. Move & update (atomic) | 45-60 min | MEDIUM |
| 4. Final validation | 15-20 min | LOW |
| **Total** | **1.5-2 hours** | **MEDIUM** |

**With buffer for issues**: 3-5 hours (as stated in TL;DR)

### Complexity Assessment

- **Directory moves**: LOW (straightforward git mv)
- **CMake updates**: MEDIUM (5 files, ~20 lines, clear patterns)
- **Include fixes**: MEDIUM (9 files, 15 lines, automated with sed)
- **Verification**: LOW (deterministic make commands)
- **Rollback**: LOW (git reset --hard available at any point)

**Overall**: MEDIUM complexity - mostly mechanical changes with clear verification at each step.

---

## Notes

### Why This Plan is Conservative

1. **Backup branch**: Created before any changes
2. **Baseline verification**: Confirm current build works before starting
3. **Sequential execution**: No parallelization, verify after each step
4. **Atomic commit for Step 3**: Move + update in one commit avoids broken intermediate state
5. **Comprehensive verification**: make configure + build + dist after each commit
6. **Clear rollback paths**: git reset available at any checkpoint

### Why Phase 3 is Skipped

Research conclusively showed legacy common/ directories are NOT duplicates of root common/:
- Different content (game structures vs utilities)
- Different purposes (reverse engineering vs helper functions)
- Correct architecture (legacy is intentionally isolated)

Phase 3 work would introduce risk with no benefit.

### Git History Preservation

All moves use `git mv` which preserves:
- Full commit history
- File blame information
- Rename detection (R100 in git log)

Verification in Step 4 confirms history is preserved.
