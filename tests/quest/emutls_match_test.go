package quest

import (
	"strings"
	"testing"
)

// emutlsSymbolPrefix is the nm-listed name of every emulated thread-local variable.
const emutlsSymbolPrefix = "__emutls_v."

// libcxxabiEmutls are the emulated thread-locals libc++abi itself defines (NDK r26d,
// toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++abi.a,
// `llvm-nm` on it). Each is matched whole, so a name that only contains one of them is refused.
var libcxxabiEmutls = map[string]bool{
	"__emutls_v._ZZN10__cxxabiv112_GLOBAL__N_19__globalsEvE10eh_globals": true, // the exception globals slot
	"__emutls_v._ZN10__cxxabiv112_GLOBAL__N_15dtorsE":                    true,
	"__emutls_v._ZN10__cxxabiv112_GLOBAL__N_111dtors_aliveE":             true,
}

// emutlsCheck returns how many emulated-TLS symbols the nm listing holds and the lines of
// those that are not libc++abi's own.
func emutlsCheck(nm string) (seen int, bad []string) {
	for _, line := range strings.Split(nm, "\n") {
		name := ""
		for _, field := range strings.Fields(line) {
			if strings.HasPrefix(field, emutlsSymbolPrefix) {
				name = field
				break
			}
		}
		if name == "" {
			continue
		}
		seen++
		if !libcxxabiEmutls[name] {
			bad = append(bad, line)
		}
	}
	return seen, bad
}

// The planted symbol is the one the q15t review used: a name in the libc++abi namespace
// that is not libc++abi's exception-globals slot.
func TestEmutlsCheckRefusesPlantedLibcxxabiNamespace(t *testing.T) {
	const planted = "0000000000000000 d __emutls_v._ZN10__cxxabiv116t_not_eh_globalsE"
	seen, bad := emutlsCheck(planted)
	if seen != 1 {
		t.Fatalf("seen = %d, want 1", seen)
	}
	if len(bad) != 1 {
		t.Errorf("planted symbol not refused: bad = %q", bad)
	}
}

// A substring of an allowed name is not the name: the check matches whole symbols.
func TestEmutlsCheckRefusesASuffixOfAnAllowedName(t *testing.T) {
	const lookalike = "0000000000000000 d __emutls_v._ZZN10__cxxabiv112_GLOBAL__N_19__globalsEvE10eh_globals_x"
	if _, bad := emutlsCheck(lookalike); len(bad) != 1 {
		t.Errorf("lookalike not refused: bad = %q", bad)
	}
}

// The three names libc++abi defines are accepted, and nothing else is seen as one of them.
func TestEmutlsCheckAcceptsLibcxxabiOwnNames(t *testing.T) {
	const nm = "0000000000000000 d __emutls_v._ZZN10__cxxabiv112_GLOBAL__N_19__globalsEvE10eh_globals\n" +
		"0000000000000000 d __emutls_v._ZN10__cxxabiv112_GLOBAL__N_15dtorsE\n" +
		"0000000000000000 d __emutls_v._ZN10__cxxabiv112_GLOBAL__N_111dtors_aliveE\n" +
		"                 U __emutls_get_address\n"
	seen, bad := emutlsCheck(nm)
	if seen != 3 {
		t.Errorf("seen = %d, want 3", seen)
	}
	if len(bad) != 0 {
		t.Errorf("libc++abi's own names refused: %q", bad)
	}
}

// An empty listing has no emulated-TLS symbol: the caller must fail on that, not pass.
func TestEmutlsCheckSeesNothingInAnEmptyListing(t *testing.T) {
	if seen, _ := emutlsCheck(""); seen != 0 {
		t.Errorf("seen = %d, want 0", seen)
	}
}
