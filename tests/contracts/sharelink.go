package contracts

import (
	"errors"
	"net/url"
	"strings"
)

// #1726: an invite code is shared as https://echo.taxi/<code>. The share page shows `echo.taxi/<code>` beside the code;
// the site resolves the path with the same normalisation the wizard applies (ADR 0017): case-insensitive, hyphens
// ignored, alphabet below. The code's length is the invite service's (ADR 0017, pending measurement); the client and the
// site bound it only at 63 characters.
const (
	InviteAlphabet = "ABCDEFGHJKMNPQRSTUVWXYZ23456789"
	shareHost      = "echo.taxi"
	maxCodeChars   = 63
)

func ShareLink(code string) string { return shareHost + "/" + code }

func ParseShareURL(raw string) (string, error) {
	u, err := url.Parse(raw)
	if err != nil || u.Scheme != "https" || u.Host != shareHost {
		return "", errors.New("not an https://echo.taxi link")
	}
	p := strings.TrimSuffix(strings.TrimPrefix(u.Path, "/"), "/")
	if p == "" || strings.Contains(p, "/") {
		return "", errors.New("the path is exactly one code")
	}
	code := strings.ToUpper(strings.ReplaceAll(p, "-", ""))
	if code == "" || len(code) > maxCodeChars {
		return "", errors.New("code length")
	}
	for _, c := range code {
		if !strings.ContainsRune(InviteAlphabet, c) {
			return "", errors.New("code symbol outside the alphabet")
		}
	}
	return code, nil
}
