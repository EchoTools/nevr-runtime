package main

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"reflect"
	"strconv"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/runlog"
	"github.com/EchoTools/nevr-runtime/tools/internal/testkit"
)

const (
	filePerm = 0o644
	dirPerm  = 0o755
	// The fake process tree: the recipe's shell, the agent session above it, the terminal multiplexer above that.
	shellPid   = 300
	sessionPid = 200
	muxPid     = 100
	initPid    = 1
	// suiteFails is the exit code the fake suite is told to return.
	suiteFails = 5
	// hookFails is the exit code the fake hook is told to return.
	hookFails = 3
	// rcName is the file in a fake's directory that holds the exit code it returns.
	rcName = "rc"
)

const sessionDir = "/home/a/seat"

func exitFromFile(c *testkit.Call) int {
	data, err := os.ReadFile(filepath.Join(c.FakeDir, rcName))
	if err != nil {
		return 0
	}
	n, _ := strconv.Atoi(strings.TrimSpace(string(data)))
	return n
}

func TestMain(m *testing.M) {
	testkit.Main(m, testkit.Fakes{"bonk": exitFromFile, "suite": exitFromFile})
}

type proc struct {
	pid, ppid    int
	program, cwd string
}

// fakeProc writes a /proc of the given processes.
func fakeProc(t *testing.T, procs ...proc) string {
	t.Helper()
	root := filepath.Join(t.TempDir(), "proc")
	for _, p := range procs {
		dir := filepath.Join(root, strconv.Itoa(p.pid))
		if err := os.MkdirAll(dir, dirPerm); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(dir, "comm"), []byte(p.program+"\n"), filePerm); err != nil {
			t.Fatal(err)
		}
		stat := strconv.Itoa(p.pid) + " (" + p.program + ") S " + strconv.Itoa(p.ppid) + " 1 1 0 -1\n"
		if err := os.WriteFile(filepath.Join(dir, "stat"), []byte(stat), filePerm); err != nil {
			t.Fatal(err)
		}
		if err := os.Symlink(p.cwd, filepath.Join(dir, "cwd")); err != nil {
			t.Fatal(err)
		}
	}
	return root
}

// agentTree is a recipe run from a shell of a Claude Code session whose directory is sessionDir.
func agentTree(t *testing.T) system {
	return system{ppid: shellPid, proc: fakeProc(t,
		proc{pid: shellPid, ppid: sessionPid, program: "zsh", cwd: "/w/checkout"},
		proc{pid: sessionPid, ppid: muxPid, program: "claude", cwd: sessionDir},
		proc{pid: muxPid, ppid: initPid, program: "tmux: server", cwd: "/home/a"})}
}

// shellTree is a recipe run from a person's terminal.
func shellTree(t *testing.T) system {
	return system{ppid: shellPid, proc: fakeProc(t,
		proc{pid: shellPid, ppid: muxPid, program: "zsh", cwd: "/w/checkout"},
		proc{pid: muxPid, ppid: initPid, program: "tmux: server", cwd: "/home/a"})}
}

type fixture struct {
	t           *testing.T
	env         []string
	work        string
	bonk, suite *testkit.FakeCommand
}

func newFixture(t *testing.T, extra ...string) *fixture {
	t.Helper()
	f := &fixture{t: t, bonk: testkit.Fake(t, "bonk"), suite: testkit.Fake(t, "suite", "NEVR_SUITE_RECIPE", "NEVR_SUITE_PARENT")}
	f.env = testkit.Env(t, append([]string{"NEVR_SUITE_ALERT=" + filepath.Join(f.bonk.Dir(), "bonk")}, extra...)...)
	f.work = testkit.Getenv(f.env)("NEVR_WORK_ROOT")
	return f
}

func (f *fixture) setExit(c *testkit.FakeCommand, code int) {
	f.t.Helper()
	if err := os.WriteFile(filepath.Join(c.Dir(), rcName), []byte(strconv.Itoa(code)), filePerm); err != nil {
		f.t.Fatal(err)
	}
}

func (f *fixture) run(sys system, recipe string) (int, string) {
	f.t.Helper()
	var out, errb bytes.Buffer
	code := run(context.Background(), []string{"-recipe", recipe, "--", filepath.Join(f.suite.Dir(), "suite"), "a", "b"}, f.env, strings.NewReader(""), &out, &errb, sys)
	return code, errb.String()
}

func (f *fixture) records() []map[string]any {
	f.t.Helper()
	fh, err := os.Open(filepath.Join(f.work, "test-runs", "suite-runs.jsonl"))
	if err != nil {
		f.t.Fatal(err)
	}
	defer fh.Close()
	var recs []map[string]any
	sc := bufio.NewScanner(fh)
	for sc.Scan() {
		var r map[string]any
		if err := json.Unmarshal(sc.Bytes(), &r); err != nil {
			f.t.Fatalf("a record is not JSON: %v: %q", err, sc.Text())
		}
		recs = append(recs, r)
	}
	return recs
}

// startEnd returns the run's two records, failing unless they are suite_start then suite_end of one run.
func (f *fixture) startEnd() (map[string]any, map[string]any) {
	f.t.Helper()
	recs := f.records()
	if len(recs) != 2 || recs[0]["event"] != "suite_start" || recs[1]["event"] != "suite_end" || recs[0]["run"] != recs[1]["run"] {
		f.t.Fatalf("records = %v, want one suite_start and one suite_end of one run", recs)
	}
	return recs[0], recs[1]
}

func TestAnAgentsRunIsRecordedAndAlertsTheOwner(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	code, errs := f.run(agentTree(t), "verify")
	if code != 0 {
		t.Fatalf("exit %d: %s", code, errs)
	}
	calls := f.bonk.Calls(t)
	if len(calls) != 1 || !reflect.DeepEqual(calls[0].Args, []string{"--from", "nevr-runtime", sessionDir + " ran verify"}) {
		t.Errorf("bonk calls = %+v, want one: --from nevr-runtime %q", calls, sessionDir+" ran verify")
	}
	start, end := f.startEnd()
	for k, want := range map[string]any{"recipe": "verify", "agent": true, "session_found": true, "session_pid": float64(sessionPid),
		"session_program": "claude", "session_cwd": sessionDir, "alert": "sent", "parent_run": "", "ppid": float64(shellPid)} {
		if start[k] != want {
			t.Errorf("suite_start %s = %v, want %v", k, start[k], want)
		}
	}
	if !reflect.DeepEqual(start["command"], []any{filepath.Join(f.suite.Dir(), "suite"), "a", "b"}) {
		t.Errorf("suite_start command = %v", start["command"])
	}
	if end["exit"] != float64(0) || end["recipe"] != "verify" {
		t.Errorf("suite_end = %v", end)
	}
	sc := f.suite.Calls(t)
	if len(sc) != 1 || sc[0].Env["NEVR_SUITE_RECIPE"] != "verify" || sc[0].Env["NEVR_SUITE_PARENT"] != start["run"] {
		t.Errorf("suite calls = %+v, want one with NEVR_SUITE_RECIPE=verify and NEVR_SUITE_PARENT=%v", sc, start["run"])
	}
}

func TestRunsThatDoNotAlert(t *testing.T) {
	t.Parallel()
	cases := []struct {
		name  string
		extra []string
		tree  func(*testing.T) system
		alert string
	}{
		{"exempt session", []string{"NEVR_SUITE_ALERT_EXEMPT=/elsewhere:" + sessionDir + "/"}, agentTree, "exempt"},
		{"a person's shell", nil, shellTree, "not_an_agent"},
		{"inside another suite run", []string{"NEVR_SUITE_PARENT=outer-run"}, agentTree, "nested"},
		{"no hook on this machine", []string{"NEVR_SUITE_ALERT="}, agentTree, "no_hook"},
	}
	for _, c := range cases {
		f := newFixture(t, c.extra...)
		code, errs := f.run(c.tree(t), "test-tools")
		if code != 0 {
			t.Errorf("%s: exit %d: %s", c.name, code, errs)
			continue
		}
		if calls := f.bonk.Calls(t); len(calls) != 0 {
			t.Errorf("%s: bonk called: %+v", c.name, calls)
		}
		start, _ := f.startEnd()
		if start["alert"] != c.alert {
			t.Errorf("%s: alert = %v, want %s", c.name, start["alert"], c.alert)
		}
		if len(f.suite.Calls(t)) != 1 {
			t.Errorf("%s: the suite did not run once", c.name)
		}
	}
}

func TestTheMarkerWithoutAnAgentAncestorIsAnAgentsRun(t *testing.T) {
	t.Parallel()
	f := newFixture(t, agentMarker+"=1")
	code, errs := f.run(shellTree(t), "test-auth-unit")
	if code != 0 {
		t.Fatalf("exit %d: %s", code, errs)
	}
	start, _ := f.startEnd()
	if start["agent"] != true || start["session_found"] != false || start["alert"] != "sent" {
		t.Errorf("suite_start = %v, want an agent's run, session not found, alert sent", start)
	}
	cwd, _ := os.Getwd()
	if calls := f.bonk.Calls(t); len(calls) != 1 || calls[0].Args[2] != cwd+" ran test-auth-unit" {
		t.Errorf("bonk calls = %+v, want one naming the run's directory %s", calls, cwd)
	}
}

func TestAFailingHookIsLoudAndTheSuiteStillRuns(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	f.setExit(f.bonk, hookFails)
	f.setExit(f.suite, suiteFails)
	code, errs := f.run(agentTree(t), "verify")
	if code != suiteFails {
		t.Errorf("exit %d, want the suite's %d", code, suiteFails)
	}
	if !strings.Contains(errs, "the alert for this verify run failed") {
		t.Errorf("stderr does not report the failed alert: %s", errs)
	}
	start, end := f.startEnd()
	if start["alert"] != "failed" || start["alert_error"] == nil {
		t.Errorf("suite_start = %v, want alert failed with alert_error", start)
	}
	if end["exit"] != float64(suiteFails) {
		t.Errorf("suite_end exit = %v, want %d", end["exit"], suiteFails)
	}
}

func TestARunThatCannotBeRecordedDoesNotStart(t *testing.T) {
	t.Parallel()
	notADir := filepath.Join(t.TempDir(), "file")
	if err := os.WriteFile(notADir, nil, filePerm); err != nil {
		t.Fatal(err)
	}
	f := newFixture(t, "NEVR_WORK_ROOT="+notADir)
	code, errs := f.run(agentTree(t), "verify")
	if code != runlog.ExitLogFailure || !strings.Contains(errs, "cannot be recorded, so it does not start") {
		t.Errorf("exit %d, stderr %s; want %d and the reason", code, errs, runlog.ExitLogFailure)
	}
	if len(f.suite.Calls(t)) != 0 || len(f.bonk.Calls(t)) != 0 {
		t.Error("the suite or the hook ran without a record")
	}
}

func TestUsage(t *testing.T) {
	t.Parallel()
	var out, errb bytes.Buffer
	if code := run(context.Background(), []string{"-recipe", "verify"}, testkit.Env(t), nil, &out, &errb, shellTree(t)); code != exitUsage {
		t.Errorf("no command: exit %d", code)
	}
	if code := run(context.Background(), []string{"--", "true"}, testkit.Env(t), nil, &out, &errb, shellTree(t)); code != exitUsage {
		t.Errorf("no recipe: exit %d", code)
	}
}
