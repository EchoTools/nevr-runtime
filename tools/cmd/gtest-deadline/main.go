// Command gtest-deadline runs one gtest program under the ceiling of tools/test_shelf.json:
//
//	gtest-deadline [-shelf file] -binary name -- program [arguments]
//
// Each test in the program is held to the ceiling (internal/gtestdeadline reads gtest's own "[ RUN      ]" and "[       OK ]"
// lines): a test that runs past it is named on standard error, the program's process group is killed and the exit code is
// 124. The program is started with --gtest_filter set to $GTEST_FILTER (everything when unset) without the shelf's gtest
// tests of -binary (ids gtest:<binary>#Suite.Test), so a shelved test is not run. The recipe test-auth-unit runs every test
// binary through it. Exit codes: the program's; 124 a test over the ceiling; 127 the program could not be started; 2 a
// flag or the shelf is wrong.
package main

import (
	"context"
	"flag"
	"fmt"
	"io"
	"os"
	"os/signal"
	"syscall"

	"github.com/EchoTools/nevr-runtime/tools/internal/gtestdeadline"
	"github.com/EchoTools/nevr-runtime/tools/internal/testshelf"
)

const (
	tool      = "gtest-deadline"
	exitUsage = 2
	// filterEnv is the variable gtest itself reads its filter from.
	filterEnv = "GTEST_FILTER"
)

func main() {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	os.Exit(run(ctx, os.Args[1:], os.Environ(), os.Getenv(filterEnv), os.Stdin, os.Stdout, os.Stderr))
}

func run(ctx context.Context, args []string, env []string, baseFilter string, stdin io.Reader, stdout, stderr io.Writer) int {
	flags := flag.NewFlagSet(tool, flag.ContinueOnError)
	flags.SetOutput(stderr)
	shelfPath := flags.String("shelf", testshelf.DefaultPath, "the test shelf")
	binary := flags.String("binary", "", "the test binary's name, as the shelf's gtest ids spell it (gtest:<binary>#Suite.Test)")
	if err := flags.Parse(args); err != nil {
		return exitUsage
	}
	if *binary == "" || flags.NArg() == 0 {
		fmt.Fprintf(stderr, "%s: usage: %s [-shelf file] -binary name -- program [arguments]\n", tool, tool)
		return exitUsage
	}
	shelf, err := testshelf.Load(*shelfPath)
	if err != nil {
		fmt.Fprintf(stderr, "%s: FAIL %v\n", tool, err)
		return exitUsage
	}
	argv := Command(shelf, *binary, baseFilter, flags.Args())
	return gtestdeadline.Run(ctx, shelf.Ceiling(), argv, env, stdin, stdout, stderr)
}

// Command is the program's argv with --gtest_filter added: baseFilter without the shelf's tests of binary. Nothing is added
// when the filter would be baseFilter as it is (the program then reads $GTEST_FILTER itself).
func Command(shelf *testshelf.Shelf, binary, baseFilter string, argv []string) []string {
	filter := testshelf.MergeGTestFilter(baseFilter, shelf.GTestExclusion(binary))
	if filter == baseFilter {
		return argv
	}
	return append(append([]string(nil), argv...), "--gtest_filter="+filter)
}
