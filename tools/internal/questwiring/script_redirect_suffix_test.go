package questwiring

import (
	"fmt"
	"regexp"
	"strconv"
	"strings"
	"testing"
)

// #2189: the redirected script library's name is built in one language and parsed in another, and its suffix is spelled in three
// places that no compiler compares: the C++ that builds the name (kTail in src/quest/plugin/script_redirect.cpp), the Python that
// stages the library under that name (REDIRECT_SUFFIX in tools/quest_repack_libs.py), and the justfile glob that collects the
// staged name into the APK (the android-repack-apk recipe). A rename in one place alone passes every test on its own side and
// breaks the redirect on a device, because the producer and the consumer no longer agree on the name. This test requires the three
// equal, and requires the name the C++ builds to be the length the host test pins (src/quest/tests/script_redirect_test.cpp), so a
// longer or shorter suffix fails here as well. Each extraction must find exactly one match: none or several fails loudly with the
// file and the pattern.

const (
	redirectCppFile  = "src/quest/plugin/script_redirect.cpp"
	redirectPyFile   = "tools/quest_repack_libs.py"
	redirectJustfile = "justfile"
	redirectHostTest = "src/quest/tests/script_redirect_test.cpp"
)

var (
	redirectTailRE     = regexp.MustCompile(`static constexpr char kTail\[\] = "([^"]*)";`)
	redirectHashRE     = regexp.MustCompile(`constexpr std::size_t kHashDigits = (\d+);`)
	redirectPySuffixRE = regexp.MustCompile(`(?m)^REDIRECT_SUFFIX = "([^"]*)"$`)
	redirectGlobRE     = regexp.MustCompile(`lib\?{16}(_[A-Za-z0-9]+\.so)`)
	redirectPinnedRE   = regexp.MustCompile(`RedirectName\(kA, buf, (\d+)\)\)\s*;\s*//\s*(\d+) characters and no room for the NUL`)
)

// redirectHit is the one match of a pattern: its file, its line and its capture groups (groups[0] is the whole match).
type redirectHit struct {
	path   string
	line   int
	groups []string
}

func (h redirectHit) at() string { return fmt.Sprintf("%s:%d", h.path, h.line) }

// findRedirectOne returns the single match of re in text, or fails the test when there is none or several.
func findRedirectOne(t *testing.T, path, text string, re *regexp.Regexp, what string) redirectHit {
	t.Helper()
	all := re.FindAllStringSubmatchIndex(text, -1)
	if len(all) != 1 {
		t.Fatalf("%s: %d matches for %s (pattern %s), want exactly 1", path, len(all), what, re)
	}
	m := all[0]
	h := redirectHit{path: path, line: strings.Count(text[:m[0]], "\n") + 1}
	for g := 0; g+1 < len(m); g += 2 {
		if m[g] < 0 {
			h.groups = append(h.groups, "")
			continue
		}
		h.groups = append(h.groups, text[m[g]:m[g+1]])
	}
	return h
}

func TestTheRedirectedLibraryNameSuffixIsTheSameInEveryPlace(t *testing.T) {
	t.Parallel()
	cpp := read(t, redirectCppFile)
	py := read(t, redirectPyFile)
	just := read(t, redirectJustfile)
	host := read(t, redirectHostTest)

	tail := findRedirectOne(t, redirectCppFile, cpp, redirectTailRE, "kTail, the C++ tail of a redirected name")
	hashDigits := findRedirectOne(t, redirectCppFile, cpp, redirectHashRE, "kHashDigits, the hex digits of the stock hash")
	pySuffix := findRedirectOne(t, redirectPyFile, py, redirectPySuffixRE, "REDIRECT_SUFFIX, the producer's suffix")
	glob := findRedirectOne(t, redirectJustfile, just, redirectGlobRE, "the staged-name glob's literal tail")
	pinned := findRedirectOne(t, redirectHostTest, host, redirectPinnedRE, "the host test's pinned name length")

	cppTail := tail.groups[1]
	if !strings.HasSuffix(cppTail, ".so") {
		t.Errorf("%s: kTail %q does not end in .so", tail.at(), cppTail)
	}
	if !strings.HasSuffix(glob.groups[1], ".so") {
		t.Errorf("%s: the glob tail %q does not end in .so", glob.at(), glob.groups[1])
	}
	cppSuffix := strings.TrimSuffix(cppTail, ".so")
	globSuffix := strings.TrimSuffix(glob.groups[1], ".so")
	pyName := pySuffix.groups[1]

	if cppSuffix != pyName {
		t.Errorf("the consumer and the producer name different suffixes: kTail at %s is %q, REDIRECT_SUFFIX at %s is %q; "+
			"the staged library is then not the one the device opens", tail.at(), cppSuffix, pySuffix.at(), pyName)
	}
	if globSuffix != pyName {
		t.Errorf("the staged-name glob and the producer name different suffixes: the glob at %s collects %q, REDIRECT_SUFFIX at %s is %q; "+
			"the APK then carries no redirected library", glob.at(), globSuffix, pySuffix.at(), pyName)
	}

	digits, err := strconv.Atoi(hashDigits.groups[1])
	if err != nil {
		t.Fatalf("%s: kHashDigits %q is not a number: %v", hashDigits.at(), hashDigits.groups[1], err)
	}
	const libPrefixLen = len("lib")
	built := libPrefixLen + digits + len(cppTail)
	want, err := strconv.Atoi(pinned.groups[1])
	if err != nil {
		t.Fatalf("%s: the pinned capacity %q is not a number: %v", pinned.at(), pinned.groups[1], err)
	}
	if said, err := strconv.Atoi(pinned.groups[2]); err != nil || said != want {
		t.Errorf("%s: the host test's comment says %s characters for the capacity %d, which disagree", pinned.at(), pinned.groups[2], want)
	}
	if built != want {
		t.Errorf("the C++ builds a %d-byte name (lib + %d hash digits + the %d-byte tail of %s, from %s), and %s pins %d; "+
			"a suffix of another length is a different name than the tests expect", built, digits, len(cppTail), tail.at(), hashDigits.at(), pinned.at(), want)
	}
}
