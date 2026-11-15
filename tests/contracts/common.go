// Package contracts holds the wire contracts between nevr-runtime and the game service (and sites) that have no code on
// the runtime side yet: one file per issue, each a strict parser or a pure decision, each pinned by its own test. A
// service written in any language checks itself against the same vectors (docs/reference/service-contracts.md).
// Standard library only.
package contracts

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"regexp"
	"strings"
	"time"
)

var (
	uuidRe   = regexp.MustCompile(`^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$`)
	sha256Re = regexp.MustCompile(`^[0-9a-f]{64}$`)
)

// strictDecode decodes one JSON value, refusing unknown fields and trailing data.
func strictDecode(b []byte, v any) error {
	d := json.NewDecoder(bytes.NewReader(b))
	d.DisallowUnknownFields()
	if err := d.Decode(v); err != nil {
		return err
	}
	if d.More() {
		return errors.New("trailing data")
	}
	return nil
}

// utcZ accepts an RFC 3339 time that ends in "Z".
func utcZ(s string) error {
	if !strings.HasSuffix(s, "Z") {
		return fmt.Errorf("time %q is not UTC with a Z", s)
	}
	_, err := time.Parse(time.RFC3339, s)
	return err
}
