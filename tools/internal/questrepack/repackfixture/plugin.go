package repackfixture

import (
	"os"
	"path/filepath"
	"strings"

	"github.com/EchoTools/nevr-runtime/tools/internal/testkit"
)

// NevrNote is C source for a plugin's NEVR_DECLARE_SUPPORTED_BUILD_IDS note with the ids, from the real macro in
// sdk/include/nevr/plugin_interface.h: the header is C++, and the bare AArch64 target here has no C++ library to include it.
func NevrNote(t testkit.T, repo, ids string) string {
	t.Helper()
	raw, err := os.ReadFile(filepath.Join(repo, "sdk", "include", "nevr", "plugin_interface.h"))
	if err != nil {
		t.Fatalf("%v", err)
	}
	lines := strings.Split(string(raw), "\n")
	start := -1
	for i, l := range lines {
		if strings.Contains(l, "#define NEVR_DECLARE_SUPPORTED_BUILD_IDS(") {
			start = i
			break
		}
	}
	if start < 0 {
		t.Fatalf("plugin_interface.h has no NEVR_DECLARE_SUPPORTED_BUILD_IDS")
	}
	end := start
	for strings.HasSuffix(strings.TrimRight(lines[end], " \t"), "\\") {
		end++
	}
	macro := "#define NEVR_NOTE_SECTION_TYPE \"%note\"\n" + strings.Join(lines[start:end+1], "\n") + "\n"
	return macro + `NEVR_DECLARE_SUPPORTED_BUILD_IDS("` + ids + `");` + "\n"
}
