package main

import (
	"bytes"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/questruntimepin"
	"github.com/EchoTools/nevr-runtime/tools/internal/questruntimepin/fixture"
	"github.com/EchoTools/nevr-runtime/tools/internal/testkit"
)

const (
	oldMD5    = fixture.OLD
	secondMD5 = fixture.SECOND
	thirdMD5  = fixture.THIRD
	// libr15Text is the file whose md5 is newMD5.
	libr15Text = "a patched libr15"
	filePerm   = 0o644
	// thirdIndex is the position of THIRD in the list.
	thirdIndex = "2"
	// The runs a runtime with eleven sites has.
	runsWithEleven = "44 immediate runs"
	stubCodeLen    = 8
	usageFailure   = 2
	absentFailure  = 3
	refusedFailure = 1
	helpText       = "hello"
)

var newMD5 = fixture.MD5Hex([]byte(libr15Text))

// world is the state of one test: a directory with the runtime to pin and the libr15 it is pinned to.
type world struct {
	t        *testing.T
	dir      string
	libvrapi string
	libr15   string
	env      []string
}

func newWorld(t *testing.T) *world {
	t.Helper()
	w := &world{t: t, dir: t.TempDir(), env: testkit.Env(t)}
	w.libvrapi = filepath.Join(w.dir, "libvrapi.so")
	w.libr15 = filepath.Join(w.dir, "libr15.so")
	w.write(w.libvrapi, fixture.MakeRuntime(oldMD5))
	w.write(w.libr15, []byte(libr15Text))
	return w
}

func (w *world) write(path string, data []byte) {
	w.t.Helper()
	if err := os.WriteFile(path, data, filePerm); err != nil {
		w.t.Fatal(err)
	}
}

func (w *world) read(path string) []byte {
	w.t.Helper()
	data, err := os.ReadFile(path)
	if err != nil {
		w.t.Fatal(err)
	}
	return data
}

func (w *world) out() string { return filepath.Join(w.dir, "pinned.so") }

type result struct {
	code           int
	stdout, stderr string
}

func (w *world) run(args ...string) result {
	w.t.Helper()
	var stdout, stderr bytes.Buffer
	code := run(w.t.Context(), args, testkit.Getenv(w.env), strings.NewReader(""), &stdout, &stderr)
	return result{code, stdout.String(), stderr.String()}
}

func (w *world) pin(extra ...string) result {
	w.t.Helper()
	return w.run(append([]string{"pin", "--libvrapi", w.libvrapi, "--libr15", w.libr15, "--out", w.out()}, extra...)...)
}

func parseRecord(t *testing.T, text string) map[string]any {
	t.Helper()
	var m map[string]any
	if err := json.Unmarshal([]byte(text), &m); err != nil {
		t.Fatalf("%q is not one JSON object: %v", text, err)
	}
	return m
}

func (w *world) requireNoOutput() {
	w.t.Helper()
	if _, err := os.Stat(w.out()); !os.IsNotExist(err) {
		w.t.Errorf("the output file exists (stat error %v)", err)
	}
}

// requireRefusal checks the contract of a refusal: exit 1, nothing on standard output, one JSON record on standard error.
func (w *world) requireRefusal(r result) map[string]any {
	w.t.Helper()
	if r.code != refusedFailure {
		w.t.Fatalf("exit %d, want %d (stderr %q)", r.code, refusedFailure, r.stderr)
	}
	if r.stdout != "" {
		w.t.Errorf("standard output %q, want none", r.stdout)
	}
	rec := parseRecord(w.t, r.stderr)
	if rec["tag"] != "NEVR.QUEST.RUNTIME_PIN" || rec["event"] != "runtime_pin_refused" {
		w.t.Errorf("refusal record = %v", rec)
	}
	return rec
}

func sha(data []byte) string { return questruntimepin.Sha256Hex(data) }

// PinCommandTest.test_pin_hashes_the_libr15_file_and_logs_one_json_line
func TestPinCommandPinHashesTheLibr15FileAndLogsOneJSONLine(t *testing.T) {
	t.Parallel()
	w := newWorld(t)
	r := w.pin()
	if r.code != 0 {
		t.Fatalf("exit %d: %s", r.code, r.stderr)
	}
	if got := w.read(w.out()); !bytes.Equal(got, fixture.MakeRuntime(newMD5)) {
		t.Error("the output is not the runtime built for the new md5")
	}
	lines := strings.Split(strings.TrimSuffix(r.stdout, "\n"), "\n")
	if len(lines) != 1 {
		t.Fatalf("%d lines on standard output: %q", len(lines), r.stdout)
	}
	rec := parseRecord(t, lines[0])
	if rec["tag"] != "NEVR.QUEST.RUNTIME_PIN" || rec["event"] != "runtime_pinned" {
		t.Errorf("tag/event = %v/%v", rec["tag"], rec["event"])
	}
	if rec["libr15_md5"] != newMD5 || rec["old_md5"] != oldMD5 || rec["new_md5"] != newMD5 {
		t.Errorf("md5s = %v, %v, %v", rec["libr15_md5"], rec["old_md5"], rec["new_md5"])
	}
	if rec["libvrapi_sha256_out"] != sha(w.read(w.out())) {
		t.Errorf("libvrapi_sha256_out = %v", rec["libvrapi_sha256_out"])
	}
	if rec["libvrapi_sha256_in"] != sha(w.read(w.libvrapi)) {
		t.Errorf("libvrapi_sha256_in = %v", rec["libvrapi_sha256_in"])
	}
	if rec["out"] != "pinned.so" {
		t.Errorf("out = %v", rec["out"])
	}
	if !bytes.Equal(w.read(w.libvrapi), fixture.MakeRuntime(oldMD5)) { // the input is left as it was
		t.Error("the input changed")
	}
}

// PinCommandTest.test_pin_twice_is_a_no_op_that_says_so
func TestPinCommandPinTwiceIsANoOpThatSaysSo(t *testing.T) {
	t.Parallel()
	w := newWorld(t)
	if r := w.pin(); r.code != 0 {
		t.Fatalf("first pin: exit %d: %s", r.code, r.stderr)
	}
	again := filepath.Join(w.dir, "again.so")
	r := w.run("pin", "--libvrapi", w.out(), "--libr15", w.libr15, "--out", again)
	if r.code != 0 {
		t.Fatalf("second pin: exit %d: %s", r.code, r.stderr)
	}
	if got := parseRecord(t, r.stdout)["event"]; got != "runtime_pin_unchanged" {
		t.Errorf("event = %v", got)
	}
	if !bytes.Equal(w.read(again), w.read(w.out())) {
		t.Error("the second pin wrote a different file")
	}
}

// PinCommandTest.test_a_refusal_exits_one_logs_the_reason_on_stderr_and_writes_nothing
func TestPinCommandARefusalExitsOneLogsTheReasonOnStderrAndWritesNothing(t *testing.T) {
	t.Parallel()
	w := newWorld(t)
	w.write(w.libvrapi, fixture.MakeRuntime(oldMD5, fixture.WithSites(questruntimepin.Sites-1)))
	rec := w.requireRefusal(w.pin())
	if reason, _ := rec["reason"].(string); !strings.Contains(reason, runsWithEleven) {
		t.Errorf("reason = %q", reason)
	}
	w.requireNoOutput()
}

// PinCommandTest.test_a_damaged_first_entry_exits_one_naming_the_expected_and_the_found_md5
func TestPinCommandADamagedFirstEntryExitsOneNamingTheExpectedAndTheFoundMD5(t *testing.T) {
	t.Parallel()
	w := newWorld(t)
	w.write(w.libvrapi, bytes.Replace(fixture.MakeRuntime(oldMD5), []byte(oldMD5), bytes.Repeat([]byte("x"), len(oldMD5)), 1))
	rec := w.requireRefusal(w.pin())
	if rec["expected_old_md5"] != oldMD5 || rec["found_md5"] != secondMD5 || rec["replace_index"] != float64(0) {
		t.Errorf("record = %v", rec)
	}
	w.requireNoOutput()
}

// PinCommandTest.test_a_wrong_expect_old_md5_exits_one
func TestPinCommandAWrongExpectOldMD5ExitsOne(t *testing.T) {
	t.Parallel()
	w := newWorld(t)
	rec := w.requireRefusal(w.pin("--expect-old-md5", thirdMD5))
	if rec["expected_old_md5"] != thirdMD5 || rec["found_md5"] != oldMD5 {
		t.Errorf("record = %v", rec)
	}
	w.requireNoOutput()
}

// PinCommandTest.test_expect_old_md5_and_replace_index_together_pin_another_entry
func TestPinCommandExpectOldMD5AndReplaceIndexTogetherPinAnotherEntry(t *testing.T) {
	t.Parallel()
	w := newWorld(t)
	r := w.pin("--replace-index", thirdIndex, "--expect-old-md5", thirdMD5)
	if r.code != 0 {
		t.Fatalf("exit %d: %s", r.code, r.stderr)
	}
	if !bytes.Equal(w.read(w.out()), fixture.MakeRuntime(oldMD5, fixture.WithOthers(secondMD5, newMD5))) {
		t.Error("the output is not the runtime with the third entry replaced")
	}
}

// PinCommandTest.test_an_out_that_is_the_input_is_refused
func TestPinCommandAnOutThatIsTheInputIsRefused(t *testing.T) {
	t.Parallel()
	w := newWorld(t)
	rec := w.requireRefusal(w.run("pin", "--libvrapi", w.libvrapi, "--libr15", w.libr15, "--out", w.libvrapi))
	if reason, _ := rec["reason"].(string); !strings.Contains(reason, "--out is the input file") {
		t.Errorf("reason = %q", reason)
	}
	if !bytes.Equal(w.read(w.libvrapi), fixture.MakeRuntime(oldMD5)) {
		t.Error("the input changed")
	}
}

// PinCommandTest.test_a_missing_libr15_is_refused
func TestPinCommandAMissingLibr15IsRefused(t *testing.T) {
	t.Parallel()
	w := newWorld(t)
	w.requireRefusal(w.run("pin", "--libvrapi", w.libvrapi, "--libr15", filepath.Join(w.dir, "none.so"), "--out", w.out()))
	w.requireNoOutput()
}

// PinCommandTest.test_detect_says_runtime_with_exit_zero_and_another_library_with_exit_three
func TestPinCommandDetectSaysRuntimeWithExitZeroAndAnotherLibraryWithExitThree(t *testing.T) {
	t.Parallel()
	w := newWorld(t)
	found := w.run("detect", "--libvrapi", w.libvrapi)
	if found.code != 0 {
		t.Fatalf("exit %d: %s", found.code, found.stderr)
	}
	rec := parseRecord(t, found.stdout)
	if rec["event"] != "runtime_detected" {
		t.Errorf("event = %v", rec["event"])
	}
	table, _ := rec["table"].([]any)
	if len(table) != 3 || table[0] != oldMD5 || table[1] != secondMD5 || table[2] != thirdMD5 {
		t.Errorf("table = %v", rec["table"])
	}
	other := filepath.Join(w.dir, "store.so")
	w.write(other, fixture.MakeElf(make([]byte, stubCodeLen), []byte("vrapi\x00")))
	absent := w.run("detect", "--libvrapi", other)
	if absent.code != absentFailure {
		t.Fatalf("exit %d, want %d", absent.code, absentFailure)
	}
	arec := parseRecord(t, absent.stdout)
	if arec["event"] != "runtime_absent" {
		t.Errorf("event = %v", arec["event"])
	}
	if got, ok := arec["table"].([]any); !ok || len(got) != 0 {
		t.Errorf("table = %v, want an empty list", arec["table"])
	}
}

// PinCommandTest.test_detect_refuses_a_file_that_is_not_an_aarch64_elf
func TestPinCommandDetectRefusesAFileThatIsNotAnAArch64ELF(t *testing.T) {
	t.Parallel()
	w := newWorld(t)
	other := filepath.Join(w.dir, "text.txt")
	w.write(other, []byte(helpText))
	w.requireRefusal(w.run("detect", "--libvrapi", other))
}

// Not a Python case: the record is the line Python's json.dumps(sort_keys=True) writes, and the run is in the run log.
func TestPinCommandRecordIsOneSortedLineAndTheRunIsLogged(t *testing.T) {
	t.Parallel()
	w := newWorld(t)
	r := w.pin()
	if r.code != 0 {
		t.Fatalf("exit %d: %s", r.code, r.stderr)
	}
	if !strings.HasPrefix(r.stdout, `{"ascii_copies": 12, "changed_bytes": `) || !strings.HasSuffix(r.stdout, "}\n") {
		t.Errorf("the line is not Python's sorted form: %q", r.stdout)
	}
	if !strings.Contains(r.stdout, `"table": ["`+oldMD5+`", "`+secondMD5+`", "`+thirdMD5+`"]`) {
		t.Errorf("the table is not written as Python writes a list: %q", r.stdout)
	}
	if strings.Count(r.stdout, "\n") != 1 || strings.Contains(r.stdout, `"tool"`) {
		t.Errorf("standard output holds more than the record: %q", r.stdout)
	}
	var logged bool
	for _, rec := range testkit.RunLogRecords(t, w.env, tool) {
		if rec["event"] == "runtime_pinned" && rec["tool"] == tool && rec["new_md5"] == newMD5 {
			logged = true
		}
	}
	if !logged {
		t.Error("the run log has no runtime_pinned record")
	}
}

// Not a Python case: argparse exits 2 on a usage error, and so does the command.
func TestPinCommandUsageErrorsExitTwo(t *testing.T) {
	t.Parallel()
	w := newWorld(t)
	for name, args := range map[string][]string{
		"no subcommand":   {},
		"unknown":         {"unpin"},
		"missing out":     {"pin", "--libvrapi", w.libvrapi, "--libr15", w.libr15},
		"bad index":       {"pin", "--libvrapi", w.libvrapi, "--libr15", w.libr15, "--out", w.out(), "--replace-index", "x"},
		"extra argument":  {"detect", "--libvrapi", w.libvrapi, "stray"},
		"detect no input": {"detect"},
	} {
		t.Run(name, func(t *testing.T) { // not parallel: the check below runs after them
			r := w.run(args...)
			if r.code != usageFailure {
				t.Errorf("exit %d, want %d (stderr %q)", r.code, usageFailure, r.stderr)
			}
			if r.stdout != "" {
				t.Errorf("standard output %q", r.stdout)
			}
		})
	}
	w.requireNoOutput()
}
