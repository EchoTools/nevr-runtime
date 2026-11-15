package contracts

import "testing"

func TestShareLinkShownAlongsideTheCode(t *testing.T) {
	if got := ShareLink("K7QX2"); got != "echo.taxi/K7QX2" {
		t.Fatal(got)
	}
}

func TestParseShareURL(t *testing.T) {
	for in, want := range map[string]string{
		"https://echo.taxi/k7qx2":   "K7QX2",
		"https://echo.taxi/K7QX2":   "K7QX2",
		"https://echo.taxi/K7-QX2":  "K7QX2",
		"https://echo.taxi/K7QX2/":  "K7QX2",
		"https://echo.taxi/K7QX2?a": "K7QX2",
	} {
		got, err := ParseShareURL(in)
		if err != nil || got != want {
			t.Errorf("%q: %q %v, want %q", in, got, err, want)
		}
	}
	for _, bad := range []string{
		"http://echo.taxi/K7QX2",
		"https://evr.echo.taxi/K7QX2",
		"https://echo.taxi/",
		"https://echo.taxi/K7Q0I",
		"https://echo.taxi/K7QX2/extra",
		"https://example.com/K7QX2",
	} {
		if got, err := ParseShareURL(bad); err == nil {
			t.Errorf("%q accepted as %q", bad, got)
		}
	}
}

func TestInviteAlphabetMatchesADR0017(t *testing.T) {
	if len(InviteAlphabet) != 31 {
		t.Fatalf("%d symbols, ADR 0017 says 31 (23 letters, 8 digits)", len(InviteAlphabet))
	}
	for _, c := range "0O1IL" {
		for _, a := range InviteAlphabet {
			if a == c {
				t.Errorf("look-alike %c in the alphabet", c)
			}
		}
	}
}
