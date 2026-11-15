// Package repackfixture is what the tests of the Quest repack share: the stand-in libr15 patcher (a fake command, ADR 0044
// rule 10), a store-like APK and a compiled shim, a fixture signing key, and the run of `just android-repack-apk` over
// them. Only test code imports it.
//
// The stand-in takes the real patcher's command line (--stock F --out F --report F [--extra-rows F]) and, by its name, a
// mode: standin-ok writes --out as the --stock bytes followed by Marker (so its output differs from its input and depends
// on it) and a --report file, and exits 0; standin-fail writes a line to standard error and exits 3; standin-nowrite exits
// 0 without writing --out; standin-empty writes an empty --out; standin-same writes --out as the --stock bytes; standin-hang
// does not finish (the caller's timeout ends it). Every call is recorded by testkit.Fake.
package repackfixture

import (
	"fmt"
	"os"
	"time"

	"github.com/EchoTools/nevr-runtime/tools/internal/testkit"
)

// Marker is what standin-ok appends to the store bytes: a newline and a line the patched file carries.
const Marker = "\n#NEVR-STANDIN-PATCHED\n"

// Command names: each mode is its own fake, so a test picks the mode by the patcher it passes.
const (
	ModeOK      = "standin-ok"
	ModeFail    = "standin-fail"
	ModeNoWrite = "standin-nowrite"
	ModeEmpty   = "standin-empty"
	ModeSame    = "standin-same"
	ModeHang    = "standin-hang"
)

// failExitCode is the code standin-fail exits with.
const failExitCode = 3

// hangFor is how long standin-hang waits before it exits, longer than any test's patcher timeout.
const hangFor = time.Minute

// reportBody is the report the stand-in writes: an empty JSON object and a newline.
const reportBody = "{}\n"

// Fakes is the map a package's TestMain gives testkit.Main, with every mode of the stand-in.
func Fakes() testkit.Fakes {
	return testkit.Fakes{
		ModeOK:      standinOK,
		ModeFail:    standinFail,
		ModeNoWrite: func(*testkit.Call) int { return 0 },
		ModeEmpty:   standinEmpty,
		ModeSame:    standinSame,
		ModeHang:    standinHang,
	}
}

// flagValue is the value of flag in args (the real patcher's command line has every flag followed by its value).
func flagValue(args []string, flag string) string {
	for i := 0; i+1 < len(args); i++ {
		if args[i] == flag {
			return args[i+1]
		}
	}
	return ""
}

func standinOK(c *testkit.Call) int {
	return writeStock(c, Marker)
}

func standinSame(c *testkit.Call) int {
	return writeStock(c, "")
}

// writeStock writes --out as the --stock bytes followed by suffix, and the report.
func writeStock(c *testkit.Call, suffix string) int {
	stock, err := os.ReadFile(flagValue(c.Args, "--stock"))
	if err != nil {
		fmt.Fprintf(c.Stderr, "standin patcher: %v\n", err)
		return 1
	}
	if err := os.WriteFile(flagValue(c.Args, "--out"), append(stock, suffix...), 0o644); err != nil {
		fmt.Fprintf(c.Stderr, "standin patcher: %v\n", err)
		return 1
	}
	if err := os.WriteFile(flagValue(c.Args, "--report"), []byte(reportBody), 0o644); err != nil {
		fmt.Fprintf(c.Stderr, "standin patcher: %v\n", err)
		return 1
	}
	return 0
}

func standinFail(c *testkit.Call) int {
	fmt.Fprint(c.Stderr, "standin patcher: refused (mode fail)\n")
	return failExitCode
}

func standinEmpty(c *testkit.Call) int {
	if err := os.WriteFile(flagValue(c.Args, "--out"), nil, 0o644); err != nil {
		fmt.Fprintf(c.Stderr, "standin patcher: %v\n", err)
		return 1
	}
	return 0
}

// standinHang waits on a timer, not a sleep: the tool that runs it kills it at its timeout.
func standinHang(*testkit.Call) int {
	<-time.After(hangFor)
	return 0
}
