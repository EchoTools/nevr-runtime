package logwiring

import (
	"strings"
	"testing"
)

// #548: both passthrough sites in builtin_filter.cpp ask the one forward predicate. The predicate itself is tested on the host
// (src/runtime/tests/passthrough_forward_rules_test.cpp); this pins that the filter calls it at both sites, and that no site still
// guards on a bare `varargs != nullptr`, which dropped every line whose format has no conversion.

const (
	passthroughFilter  = "src/runtime/log/builtin_filter.cpp"
	canForwardCall     = "nevr_passthrough_forward::CanForward(fmt, varargs)"
	bareVarargsGuard   = "varargs != nullptr"
	passthroughSites   = 2
	noBareVarargsGuard = 0
)

func TestBothPassthroughSitesCallTheForwardPredicate(t *testing.T) {
	src := code(t, passthroughFilter)
	if got := strings.Count(src, canForwardCall); got != passthroughSites {
		t.Errorf("%s has %d calls of %q, want %d (one per passthrough site, #548)", passthroughFilter, got, canForwardCall, passthroughSites)
	}
}

func TestNoPassthroughSiteGuardsOnABareNullVarargsCheck(t *testing.T) {
	src := code(t, passthroughFilter)
	if n := strings.Count(src, bareVarargsGuard); n != noBareVarargsGuard {
		t.Errorf("%s has %d bare %q guards; each drops a line whose format has no conversion (#548)", passthroughFilter, n, bareVarargsGuard)
	}
}
