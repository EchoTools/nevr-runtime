package main

import (
	"bytes"
	"strings"
	"testing"
)

const validJSON = `{
  "0x00027229b6c95edd": "section01_first_speed_update",
  "0x0000b27f29ed0f87": "mpl_cbt_gauss_s03_scaffolding_rail_b~instance_phys"
}`

func TestLoadCorpusValid(t *testing.T) {
	corpus, err := LoadCorpus(strings.NewReader(validJSON))
	if err != nil {
		t.Fatalf("LoadCorpus: %v", err)
	}
	if len(corpus) != 2 {
		t.Fatalf("entries = %d, want 2", len(corpus))
	}
	if corpus[0x00027229b6c95edd] != "section01_first_speed_update" {
		t.Fatalf("missing entry: %v", corpus)
	}
}

func TestLoadCorpusRejectsBadInput(t *testing.T) {
	cases := []struct {
		name string
		body string
		want string
	}{
		{"not json", `{`, "parse"},
		{"missing 0x", `{"00027229b6c95edd": "x"}`, "key"},
		{"uppercase hex", `{"0x00027229B6C95EDD": "x"}`, "key"},
		{"short key", `{"0x00027229b6c95ed": "x"}`, "key"},
		{"non-string value", `{"0x00027229b6c95edd": 7}`, "string"},
		{"null value", `{"0x00027229b6c95edd": null}`, "string"},
		{"duplicate key", `{"0x00027229b6c95edd": "a", "0x00027229b6c95edd": "b"}`, "duplicate"},
		{"trailing junk", validJSON + ` x`, "trailing"},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			_, err := LoadCorpus(strings.NewReader(c.body))
			if err == nil {
				t.Fatalf("LoadCorpus accepted %q", c.body)
			}
			if !strings.Contains(strings.ToLower(err.Error()), c.want) {
				t.Fatalf("error %q does not mention %q", err, c.want)
			}
		})
	}
}

func TestValidateRejectsNameHashMismatch(t *testing.T) {
	corpus := Corpus{0x00027229b6c95edd: "not-the-name"}
	err := ValidateCorpus(corpus, ValidateOptions{})
	if err == nil || !strings.Contains(err.Error(), "0x00027229b6c95edd") {
		t.Fatalf("ValidateCorpus = %v, want a mismatch naming the hash", err)
	}
}

func TestValidateCountDelta(t *testing.T) {
	makeCorpus := func(n int) Corpus {
		c := Corpus{}
		names := []string{"a", "b", "c", "d", "e", "f", "g", "h", "i", "j"}
		for i := 0; i < n; i++ {
			name := names[i%len(names)] + strings.Repeat("x", i/len(names))
			c[csymbol64(name)] = name
		}
		return c
	}
	if err := ValidateCorpus(makeCorpus(120), ValidateOptions{PreviousCount: 100}); err != nil {
		t.Fatalf("120 vs 100 should be within 20%%: %v", err)
	}
	if err := ValidateCorpus(makeCorpus(121), ValidateOptions{PreviousCount: 100}); err == nil {
		t.Fatal("121 vs 100 is more than 20%, want error")
	}
	if err := ValidateCorpus(makeCorpus(80), ValidateOptions{PreviousCount: 100}); err != nil {
		t.Fatalf("80 vs 100 should be within 20%%: %v", err)
	}
	if err := ValidateCorpus(makeCorpus(79), ValidateOptions{PreviousCount: 100}); err == nil {
		t.Fatal("79 vs 100 is more than 20%, want error")
	}
	if err := ValidateCorpus(makeCorpus(5), ValidateOptions{PreviousCount: 0}); err != nil {
		t.Fatalf("no previous count should skip the delta check: %v", err)
	}
}

func TestWriteGeneratedSortedDeterministicAndEscaped(t *testing.T) {
	corpus := Corpus{
		0x00027229b6c95edd: "section01_first_speed_update",
		0x0000b27f29ed0f87: "mpl_cbt_gauss_s03_scaffolding_rail_b~instance_phys",
		0x6d61808f947d0a63: "quote\"backslash\\name",
	}
	var first, second bytes.Buffer
	if err := WriteGenerated(&first, corpus, "echo_vr-pc.json", "27fd4ce81e1fc64315dcb4cf935086b3f80c2d81"); err != nil {
		t.Fatal(err)
	}
	if err := WriteGenerated(&second, corpus, "echo_vr-pc.json", "27fd4ce81e1fc64315dcb4cf935086b3f80c2d81"); err != nil {
		t.Fatal(err)
	}
	if first.String() != second.String() {
		t.Fatal("output is not deterministic")
	}
	out := first.String()
	if !strings.Contains(out, "kSymCacheCount = 3;") {
		t.Fatalf("missing count:\n%s", out)
	}
	low := strings.Index(out, "0x0000b27f29ed0f87")
	high := strings.Index(out, "0x6d61808f947d0a63")
	if low < 0 || high < 0 || low > high {
		t.Fatalf("entries are not sorted by hash:\n%s", out)
	}
	if !strings.Contains(out, `quote\"backslash\\name`) {
		t.Fatalf("value not C-escaped:\n%s", out)
	}
	if !strings.Contains(out, "27fd4ce81e1fc64315dcb4cf935086b3f80c2d81") {
		t.Fatalf("pin not in header:\n%s", out)
	}
}

func TestParseGeneratedAcceptsUppercaseHexHash(t *testing.T) {
	// The nakama-era generated file carried two uppercase hashes; parsing the
	// committed table for diffing must not silently drop them (6,028 removed,
	// not 6,026).
	body := "extern const SymCacheEntry kSymCacheData[] = {\n" +
		"    {0x7777777777770A00ULL, \"ERGameServerChallengeRequest\"},\n" +
		"    {0xA1B9CAE1F8588968ULL, \"SNSLobbyPlayerSessionsSuccessv2\"},\n" +
		"};\n"
	corpus, err := ParseGenerated(strings.NewReader(body))
	if err != nil {
		t.Fatalf("ParseGenerated: %v", err)
	}
	if len(corpus) != 2 {
		t.Fatalf("parsed %d entries, want 2", len(corpus))
	}
	if corpus[0x7777777777770A00] != "ERGameServerChallengeRequest" {
		t.Fatalf("uppercase hash missing: %v", corpus)
	}
}

func TestParseGeneratedRoundTrips(t *testing.T) {
	corpus := Corpus{0x0000b27f29ed0f87: "mpl_cbt_gauss_s03_scaffolding_rail_b~instance_phys"}
	var buf bytes.Buffer
	if err := WriteGenerated(&buf, corpus, "echo_vr-pc.json", "27fd4ce81e1fc64315dcb4cf935086b3f80c2d81"); err != nil {
		t.Fatal(err)
	}
	got, err := ParseGenerated(&buf)
	if err != nil {
		t.Fatalf("ParseGenerated: %v", err)
	}
	if got[0x0000b27f29ed0f87] != corpus[0x0000b27f29ed0f87] {
		t.Fatalf("round trip = %v", got)
	}
}
