package logwiring

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// #548: the S31 row of tests/smoke/score-log.sh scores a failed console-handler install. Its fail pattern has to match the line the
// runtime writes, or a failed install scores ABSENT and the script exits 0. The sample is built from the runtime's own format string,
// so a change to either side that stops the match fails here.

const (
	scoreLog          = "tests/smoke/score-log.sh"
	s31Row            = "S31@@"
	scoreFieldSep     = "@@"
	s31FailField      = 4
	s31RuntimeSource  = "src/runtime/lifecycle/crash_recovery.cpp"
	s31RuntimeLead    = "[NEVR.PATCH] SetConsoleCtrlHandler install FAILED err="
	s31ErrorConv      = "%lu"
	s31SampleErrorNum = "5"
)

func TestScoreLogS31FailPatternMatchesTheRuntimeLine(t *testing.T) {
	b, err := os.ReadFile(filepath.Join(root(t), scoreLog))
	if err != nil {
		t.Fatal(err)
	}
	script := string(b)
	var fail string
	for _, line := range strings.Split(script, "\n") {
		if !strings.HasPrefix(line, s31Row) {
			continue
		}
		fields := strings.Split(line, scoreFieldSep)
		if len(fields) <= s31FailField {
			t.Fatalf("%s: row %q has %d fields, want a fail pattern at field %d", scoreLog, line, len(fields), s31FailField)
		}
		fail = fields[s31FailField]
	}
	if fail == "" {
		t.Fatalf("%s has no S31 row with a fail pattern", scoreLog)
	}

	runtime := code(t, s31RuntimeSource)
	at := strings.Index(runtime, s31RuntimeLead+s31ErrorConv)
	if at < 0 {
		t.Fatalf("%s no longer writes %q", s31RuntimeSource, s31RuntimeLead+s31ErrorConv)
	}
	sample := s31RuntimeLead + s31SampleErrorNum
	re, err := regexp.Compile(fail)
	if err != nil {
		t.Fatalf("%s S31 fail pattern %q does not compile: %v", scoreLog, fail, err)
	}
	if !re.MatchString(sample) {
		t.Errorf("%s S31 fail pattern %q does not match the runtime line %q (#548)", scoreLog, fail, sample)
	}
}
