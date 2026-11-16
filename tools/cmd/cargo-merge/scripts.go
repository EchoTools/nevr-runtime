package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"os"
	"path/filepath"
	"strings"

	"github.com/klauspost/compress/zstd"

	"github.com/EchoTools/nevr-runtime/tools/internal/scriptgate"
)

// scriptCheck is the script_undelivered check over the added labels, each with its own `scripts` list added to the gate. A
// record's script comes from its name; the rows of a level
// table come from its blob under blobs, when one is given. A level table whose blob is not given cannot be read, and refuses
// the merge, naming each such table, unless allowUnchecked is set: then the merge goes on and a Warning names the tables.
func scriptCheck(gate scriptgate.Gate, blobs string, allowUnchecked bool, log *slog.Logger) addCheck {
	return func(add *manifestIn) ([]string, error) {
		var unchecked []string
		var refusals []error
		for _, l := range add.labels {
			recs, err := labelRecords(l, blobs)
			if err != nil {
				return nil, fmt.Errorf("label %s: %w", l.name, err)
			}
			res, err := gate.WithListed(l.name, l.listed).Check(l.name, recs)
			if err != nil {
				return nil, err
			}
			for _, name := range res.Unchecked {
				unchecked = append(unchecked, l.name+"/"+scriptgate.Hex(name))
			}
			if ref := scriptgate.Refusal(l.name, res.Undelivered); ref != nil {
				refusals = append(refusals, ref)
			}
		}
		if len(unchecked) > 0 {
			tables := strings.Join(unchecked, ",")
			if !allowUnchecked {
				refusals = append(refusals, fmt.Errorf("level tables %s were not read: their blobs are not under -add-blobs, so their CScriptCR rows cannot be checked for %s; give -add-blobs <dir>, or -allow-unchecked-tables to merge them unchecked", tables, scriptgate.Reason))
			} else {
				log.Warn("script tables unchecked", "tables", tables, "count", len(unchecked),
					"consequence", "-allow-unchecked-tables is set: these tables' CScriptCR rows were not checked for "+scriptgate.Reason+", so a row naming an undelivered script is merged")
			}
		}
		return unchecked, errors.Join(refusals...)
	}
}

// labelRecords is the label's records that the check reads: every record of a script type or of the level table type. An
// entry of the table type carries its decoded blob when blobs is set; an empty record carries no bytes.
func labelRecords(l labelIn, blobs string) ([]scriptgate.Record, error) {
	var recs []scriptgate.Record
	for _, raw := range l.entries {
		var e struct {
			Platform string `json:"platform"`
			Type     string `json:"type"`
			Name     string `json:"name"`
			Sha256   string `json:"sha256"`
		}
		if err := json.Unmarshal(raw, &e); err != nil {
			return nil, err
		}
		typ, name, err := parseKey(e.Type, e.Name)
		if err != nil {
			return nil, err
		}
		if typ != scriptgate.ScriptType && typ != scriptgate.TableType {
			continue
		}
		r := scriptgate.Record{Platform: e.Platform, Type: typ, Name: name}
		if typ == scriptgate.TableType && blobs != "" {
			if r.Data, err = readBlob(blobs, e.Sha256); err != nil {
				return nil, fmt.Errorf("level table %s: %w", e.Name, err)
			}
		}
		recs = append(recs, r)
	}
	for _, e := range l.empty {
		typ, name, err := parseKey(e.Type, e.Name)
		if err != nil {
			return nil, err
		}
		if typ == scriptgate.ScriptType || typ == scriptgate.TableType {
			recs = append(recs, scriptgate.Record{Platform: e.Platform, Type: typ, Name: name, Data: []byte{}})
		}
	}
	return recs, nil
}

func parseKey(typ, name string) (uint64, uint64, error) {
	t, err := scriptgate.ParseHex(typ)
	if err != nil {
		return 0, 0, fmt.Errorf("type: %w", err)
	}
	n, err := scriptgate.ParseHex(name)
	if err != nil {
		return 0, 0, fmt.Errorf("name: %w", err)
	}
	return t, n, nil
}

// readBlob reads blobs/sha256/<hash>, checks the frame against its name, and decodes it.
func readBlob(dir, hash string) ([]byte, error) {
	if !hexSha.MatchString(hash) {
		return nil, fmt.Errorf("sha256 %q is not %d lowercase hex digits", hash, sha256HexLen)
	}
	frame, err := os.ReadFile(filepath.Join(dir, "blobs", "sha256", hash))
	if err != nil {
		return nil, err
	}
	if got := sha256Hex(frame); got != hash {
		return nil, fmt.Errorf("blob %s hashes to %s", hash, got)
	}
	dec, err := zstd.NewReader(nil)
	if err != nil {
		return nil, err
	}
	defer dec.Close()
	out, err := dec.DecodeAll(frame, []byte{})
	if err != nil {
		return nil, fmt.Errorf("blob %s does not decode: %w", hash, err)
	}
	return out, nil
}
