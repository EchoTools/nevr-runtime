# Debugging Methodology

This document codifies the debugging approach to be used when investigating issues in this codebase.

## Core Principles

### 1. Don't Assume

Never assume what the code is doing. When you catch yourself saying things like "the process exited on its own" or "it probably crashed because...", stop. You don't know that. You're guessing.

### 2. You Cannot Debug By Inspection

> "Debugging is twice as hard as writing the code in the first place. Therefore, if you write the code as cleverly as possible, you are, by definition, not smart enough to debug it." - Brian Kernighan

If you're operating at your maximum skill level when writing code, you are by definition incapable of debugging it just by reading it. Don't try to trace through complex call chains in your head and declare "I found it." You haven't.

### 3. Add Instrumentation To Validate Assumptions

Instead of guessing what's happening:
1. Identify your assumption (e.g., "the context is being cancelled by X")
2. Add debug logging at the specific points that will prove or disprove that assumption
3. Run the code
4. Read the logs
5. Now you know

Example: If you think a context is being cancelled somewhere in the call chain, add logging at each point where:
- The context is received
- The context could be cancelled
- The context's Done() channel fires

### 4. Ask Before Acting

Don't go off investigating on your own. When debugging:
1. Propose what you want to look at and why
2. Discuss the approach
3. Then do it

This prevents wasted cycles chasing wrong assumptions.

### 5. Know How To Verify Your Changes

Before running any test:
- Know exactly how you'll confirm the test ran with your new code
- Check PIDs match
- Check timestamps in logs correspond to your test
- Confirm your debug log lines actually appear

If you rebuild code but the old binary is still running, your debug logs won't appear and you'll waste time confused about why.

### 6. Think Through The Plan

Before adding instrumentation:
- Where exactly will you add the logs?
- Why those specific locations?
- What will each log line tell you?
- What's the expected output if your assumption is correct?
- What's the expected output if your assumption is wrong?

## Practical Checklist

When investigating a bug:

- [ ] State your hypothesis clearly
- [ ] Identify what evidence would prove/disprove it
- [ ] Add logging at specific points to gather that evidence
- [ ] Verify the instrumented code is actually running (check PID, timestamps)
- [ ] Run the test case
- [ ] Read the actual log output
- [ ] Update your hypothesis based on evidence, not speculation
- [ ] Repeat until root cause is found

## Anti-Patterns

**Never:**
- Say "it probably..." without evidence
- Trace through code in your head and declare the bug found
- Run tests without confirming your changes are live
- Add logging without knowing what you expect to see
- Make multiple changes at once (you won't know which one mattered)

**Always:**
- Gather evidence before drawing conclusions
- Verify instrumentation is running before trusting its output
- One hypothesis, one test, one conclusion at a time
