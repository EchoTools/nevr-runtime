package questwiring

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// #2189: the reconstruction sources written for the Quest gun and drone call surface carry no name of the deleted q15t plugin.
// docs/standards/naming.md: a namespace the reconstruction wrote is `NRadEngine::nevr_reconstruction` (the game's prefix, then
// `nevr_<area>`). Sixteen files under src/reconstruction/r15 opened `q15t::r15::reconstruction` instead (the plugin's name, a PC/Quest
// word the namespace rule has no place for). They include each other through cnca_game.hpp, so a file left on the old namespace
// would not see the shared types: the library would stop building, or it would build with the types declared twice.
// This file is the Go port of tools/tests/test_reconstruction_namespace.py, with the same assertions.

const reconstructionNamespace = "NRadEngine::nevr_reconstruction"

var (
	reconstructionFiles = []string{
		"cactor_alias_cs_resolve.cpp",
		"clocal_component_handle_tag_find.cpp",
		"cnca_game.hpp",
		"cnca_game_loaded_space.cpp",
		"cnca_game_space.cpp",
		"cnca_game_space_wrapper.cpp",
		"cr15_echo_unit_base_transform.cpp",
		"cr15_game_space_mt.cpp",
		"cr15_net_follow_player_cs_follow.cpp",
		"cr15_net_game_user_actor.cpp",
		"cr15_player_nav_final_transform.cpp",
		"cr15_player_nav_is_component_valid.cpp",
		"cr15_resolve_actor_alias.cpp",
		"ctag_cs_has_tag.cpp",
		"ctransfqs_append.cpp",
		"fill_resource_file_request.cpp",
	}
	nsOpenRE  = regexp.MustCompile(`(?m)^namespace\s+([A-Za-z0-9_:]+)\s*\{\s*$`)
	nsCloseRE = regexp.MustCompile(`(?m)^\}\s*//\s*namespace\s+([A-Za-z0-9_:]+)\s*$`)
)

// namespaceNames returns the name each match of re captures, in text order.
func namespaceNames(re *regexp.Regexp, text string) []string {
	var out []string
	for _, m := range re.FindAllStringSubmatch(text, -1) {
		out = append(out, m[1])
	}
	return out
}

func countOf(list []string, s string) int {
	n := 0
	for _, x := range list {
		if x == s {
			n++
		}
	}
	return n
}

func TestTheSixteenReconstructionFilesExist(t *testing.T) {
	t.Parallel()
	for _, name := range reconstructionFiles {
		if !exists(t, "src/reconstruction/r15/"+name) {
			t.Errorf("src/reconstruction/r15/%s does not exist", name)
		}
	}
}

func TestEachReconstructionFileOpensAndClosesTheOneNamespaceOnce(t *testing.T) {
	t.Parallel()
	for _, name := range reconstructionFiles {
		text := read(t, "src/reconstruction/r15/"+name)
		if n := countOf(namespaceNames(nsOpenRE, text), reconstructionNamespace); n != 1 {
			t.Errorf("%s: %d of `namespace %s {`, want one", name, n, reconstructionNamespace)
		}
		if n := countOf(namespaceNames(nsCloseRE, text), reconstructionNamespace); n != 1 {
			t.Errorf("%s: %d of `} // namespace %s`, want one", name, n, reconstructionNamespace)
		}
	}
}

func TestNoReconstructionSourceNamesTheOldNamespaceOrAQ15TNamespace(t *testing.T) {
	t.Parallel()
	var sources []string
	for _, f := range trackedFiles(t, "src/reconstruction") {
		if strings.HasSuffix(f, ".cpp") || strings.HasSuffix(f, ".hpp") || strings.HasSuffix(f, ".h") {
			sources = append(sources, f)
		}
	}
	if len(sources) <= len(reconstructionFiles) {
		t.Fatalf("only %d tracked reconstruction sources, want more than the %d named files: the listing is broken", len(sources), len(reconstructionFiles))
	}
	for _, p := range sources {
		b, err := os.ReadFile(filepath.Join(root(t), p))
		if err != nil {
			t.Fatal(err)
		}
		text := string(b)
		if strings.Contains(text, "q15t::") {
			t.Errorf("%s still names a q15t namespace", p)
		}
		for _, name := range append(namespaceNames(nsOpenRE, text), namespaceNames(nsCloseRE, text)...) {
			if anyQ15tRE.MatchString(name) {
				t.Errorf("%s: namespace %s names q15t", p, name)
			}
			if strings.Contains(name, "r15::reconstruction") {
				t.Errorf("%s: namespace %s is the old r15::reconstruction", p, name)
			}
		}
	}
}
