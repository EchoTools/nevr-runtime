// Package testkit holds the things every test of a tool repeats: a hermetic environment, fake external commands, waits
// with a deadline, and a fifo to wait on. Standard library only (ADR 0044).
//
// The pieces:
//
//   - Env(t, extra...) is the environment to give every command a test starts: nothing of the user's configuration, git
//     with a fixed identity and no signing, and HOME, XDG_*_HOME, NEVR_WORK_ROOT and TMPDIR under t.TempDir().
//     WithPath puts a directory first on its PATH; Getenv reads one as the func(string) string a tool takes.
//   - Main(m, fakes) in a package's TestMain makes the test binary a set of fake commands; Fake(t, name) links it under
//     a command's name, records each call, and returns the directory to put on PATH.
//   - WaitFor and WaitLines block on a condition or a fifo until a deadline taken from the test's context, and fail the
//     test with what they waited for. Within sets a longer or shorter budget. Nothing here sleeps.
//
// A test that waits for something in its own process, or that tests a timeout, uses testing/synctest, not this package.
// A fake command:
//
//	func TestMain(m *testing.M) {
//		testkit.Main(m, testkit.Fakes{"gh": func(c *testkit.Call) int {
//			fmt.Fprintln(c.Stdout, `{"number": 7}`)
//			return 0
//		}})
//	}
//
//	func TestPrOpened(t *testing.T) {
//		t.Parallel()
//		gh := testkit.Fake(t, "gh", "GH_TOKEN")
//		env := testkit.WithPath(testkit.Env(t, "GH_TOKEN=x"), gh.Dir())
//		// ... start the tool with env, then:
//		if calls := gh.Calls(t); len(calls) != 1 { /* ... */ }
//	}
package testkit
