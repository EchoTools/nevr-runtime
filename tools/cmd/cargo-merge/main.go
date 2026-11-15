// Command cargo-merge joins a base content manifest and one added manifest into one manifest in the schema of
// src/cdn/cdn_manifest.h (docs/adr/0045-cdn-texture-delivery.md). The base's labels keep their exact bytes and their
// order, and come first: the merge never re-encodes them, so a label's position in the document (which the budget of a
// device follows, src/cdn/cdn_plan.cpp) and its bytes do not change. The added manifest's labels follow, in its order.
//
//	cargo-merge -base <manifest.json.zst> -base-sha256 <hex> -add <manifest.json.zst> -add-sha256 <hex> -out <dir>
//
// Both inputs are zstd frames pinned by their sha256: a frame that does not hash to its pin is refused before it is
// read. Input keys the tool does not know are refused, so nothing is dropped. A label name in both inputs is refused, and
// so is a (platform, type, name) listed twice across them. The added manifest's top-level empty records move into its one
// label, with that label's one platform; an added manifest with top-level empties and other than one label or one
// platform is refused. The base has no top-level empty records to move.
//
// The merged manifest is written as one zstd frame at <dir>/manifest.json.zst, which is refused when it exists. Its sha256
// is printed on standard output as one JSON line. The merge writes no object, so the blobs of the added set are not
// copied here.
//
// Exit codes: 0 done; 1 refused; 2 usage; 74 the run log failed (internal/runlog).
package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log/slog"
	"math/bits"
	"os"
	"path/filepath"
	"regexp"

	"github.com/klauspost/compress/zstd"

	"github.com/EchoTools/nevr-runtime/tools/internal/runlog"
)

const (
	schemaName    = "nevr-cargo-manifest"
	schemaVersion = 1
	manifestName  = "manifest.json.zst"
	sha256HexLen  = 2 * sha256.Size
	exitRefused   = 1
	exitUsage     = 2
	dirPerm       = 0o755
	filePerm      = 0o644
	keySchema     = "schema"
	keySchemaVer  = "schema_version"
	keyLabels     = "labels"
	keyEntries    = "entries"
	keyEmpty      = "empty"
)

var hexSha = regexp.MustCompile(fmt.Sprintf("^[0-9a-f]{%d}$", sha256HexLen))

// emptyDoc is a record of 0 bytes: there is no object for it, and it names its platform in its label.
type emptyDoc struct {
	Platform string `json:"platform"`
	Type     string `json:"type"`
	Name     string `json:"name"`
	Align    uint32 `json:"align"`
}

// entryKey is the identity of a record across the two manifests.
type entryKey struct {
	Platform string `json:"platform"`
	Type     string `json:"type"`
	Name     string `json:"name"`
}

// labelIn is one label of an input, with its value's exact bytes.
type labelIn struct {
	name    string
	raw     json.RawMessage
	entries []json.RawMessage
	keys    []entryKey
	empty   []emptyDoc
}

// manifestIn is an input manifest in document order.
type manifestIn struct {
	labels []labelIn
	empty  []emptyDoc
}

// labelReport counts one label of the merged manifest.
type labelReport struct {
	Name    string `json:"label"`
	Entries int    `json:"entries"`
	Empty   int    `json:"empty"`
}

// mergeResult is the merged manifest: its frame, the frame's sha256 and the text it frames.
type mergeResult struct {
	Frame  []byte
	Sha256 string
	Text   []byte
	Labels []labelReport
}

// summary is the one line cargo-merge prints.
type summary struct {
	Labels         []labelReport `json:"labels"`
	ManifestSha256 string        `json:"manifest_sha256"`
	ManifestBytes  int           `json:"manifest_frame_bytes"`
	TextBytes      int           `json:"manifest_text_bytes"`
}

func main() {
	os.Exit(runlog.Run("cargo-merge", os.Getenv, os.Stderr, func(log *slog.Logger) int {
		return run(os.Args[1:], os.Stdout, os.Stderr, log)
	}))
}

func sha256Hex(b []byte) string {
	s := sha256.Sum256(b)
	return hex.EncodeToString(s[:])
}

func run(args []string, stdout, stderr io.Writer, log *slog.Logger) int {
	fs := flag.NewFlagSet("cargo-merge", flag.ContinueOnError)
	fs.SetOutput(stderr)
	basePath := fs.String("base", "", "the base manifest frame (its labels keep their bytes and come first)")
	baseSha := fs.String("base-sha256", "", "the sha256 the base frame must hash to")
	addPath := fs.String("add", "", "the added manifest frame")
	addSha := fs.String("add-sha256", "", "the sha256 the added frame must hash to")
	out := fs.String("out", "", "the output directory; the merged manifest is written as "+manifestName)
	if err := fs.Parse(args); err != nil {
		return exitUsage
	}
	if *basePath == "" || *addPath == "" || *out == "" || fs.NArg() != 0 || !hexSha.MatchString(*baseSha) || !hexSha.MatchString(*addSha) {
		fmt.Fprintln(stderr, "usage: cargo-merge -base <frame> -base-sha256 <hex> -add <frame> -add-sha256 <hex> -out <dir>")
		return exitUsage
	}
	baseFrame, err := os.ReadFile(*basePath)
	if err != nil {
		return refuse(stderr, log, err)
	}
	addFrame, err := os.ReadFile(*addPath)
	if err != nil {
		return refuse(stderr, log, err)
	}
	res, err := merge(baseFrame, *baseSha, addFrame, *addSha)
	if err != nil {
		return refuse(stderr, log, err)
	}
	if err := writeOnce(filepath.Join(*out, manifestName), res.Frame); err != nil {
		return refuse(stderr, log, err)
	}
	line, err := json.Marshal(summary{Labels: res.Labels, ManifestSha256: res.Sha256, ManifestBytes: len(res.Frame), TextBytes: len(res.Text)})
	if err != nil {
		return refuse(stderr, log, err)
	}
	fmt.Fprintln(stdout, string(line))
	log.Info("merged", "labels", len(res.Labels), "manifest_sha256", res.Sha256)
	return 0
}

func refuse(stderr io.Writer, log *slog.Logger, err error) int {
	fmt.Fprintln(stderr, "cargo-merge:", err)
	log.Error("refused", "err", err.Error())
	return exitRefused
}

// writeOnce creates the file and refuses to replace one that exists.
func writeOnce(path string, data []byte) error {
	if err := os.MkdirAll(filepath.Dir(path), dirPerm); err != nil {
		return err
	}
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, filePerm)
	if err != nil {
		return fmt.Errorf("%s: %w", path, err)
	}
	if _, err := f.Write(data); err != nil {
		f.Close()
		return err
	}
	return f.Close()
}

// framedText checks a frame against its pinned sha256 and returns the text it frames.
func framedText(frame []byte, pinned, which string) ([]byte, error) {
	if got := sha256Hex(frame); got != pinned {
		return nil, fmt.Errorf("the %s frame has sha256 %s, not the pinned sha256 %s: refused before it is read", which, got, pinned)
	}
	dec, err := zstd.NewReader(nil)
	if err != nil {
		return nil, err
	}
	defer dec.Close()
	text, err := dec.DecodeAll(frame, nil)
	if err != nil {
		return nil, fmt.Errorf("the %s frame does not decode: %w", which, err)
	}
	return text, nil
}

// merge joins the two pinned frames into one manifest frame.
func merge(baseFrame []byte, baseSha string, addFrame []byte, addSha string) (*mergeResult, error) {
	baseText, err := framedText(baseFrame, baseSha, "base")
	if err != nil {
		return nil, err
	}
	addText, err := framedText(addFrame, addSha, "added")
	if err != nil {
		return nil, err
	}
	base, err := parseManifest(baseText, "base")
	if err != nil {
		return nil, err
	}
	add, err := parseManifest(addText, "added")
	if err != nil {
		return nil, err
	}
	if len(base.empty) != 0 {
		return nil, errors.New("the base has top-level empty records: cargo-merge moves only the added set's empties")
	}
	if err := moveTopLevelEmpties(&add); err != nil {
		return nil, err
	}
	if err := checkCollisions(base, add); err != nil {
		return nil, err
	}
	text, reports, err := encodeMerged(base, add)
	if err != nil {
		return nil, err
	}
	frame, err := encodeFrame(text)
	if err != nil {
		return nil, fmt.Errorf("manifest frame: %w", err)
	}
	return &mergeResult{Frame: frame, Sha256: sha256Hex(frame), Text: text, Labels: reports}, nil
}

// parseManifest reads a manifest in document order. A top-level key it does not know is refused, and so is a label
// name written twice; the label values are kept as their exact bytes.
func parseManifest(text []byte, which string) (manifestIn, error) {
	var m manifestIn
	dec := json.NewDecoder(bytes.NewReader(text))
	if err := expectDelim(dec, '{'); err != nil {
		return m, fmt.Errorf("%s: %w", which, err)
	}
	var schema string
	var version int
	for dec.More() {
		k, err := keyOf(dec)
		if err != nil {
			return m, fmt.Errorf("%s: %w", which, err)
		}
		switch k {
		case keySchema:
			err = dec.Decode(&schema)
		case keySchemaVer:
			err = dec.Decode(&version)
		case keyLabels:
			m.labels, err = parseLabels(dec)
		case keyEmpty:
			err = dec.Decode(&m.empty)
		default:
			return m, fmt.Errorf("%s: top-level key %q is not one cargo-merge knows, so it is refused rather than dropped", which, k)
		}
		if err != nil {
			return m, fmt.Errorf("%s: %s: %w", which, k, err)
		}
	}
	if err := expectDelim(dec, '}'); err != nil {
		return m, fmt.Errorf("%s: %w", which, err)
	}
	if schema != schemaName || version != schemaVersion {
		return m, fmt.Errorf("%s: schema %q version %d, want %q version %d", which, schema, version, schemaName, schemaVersion)
	}
	return m, nil
}

// parseLabels reads the labels object in order, keeping each label's bytes.
func parseLabels(dec *json.Decoder) ([]labelIn, error) {
	if err := expectDelim(dec, '{'); err != nil {
		return nil, err
	}
	var labels []labelIn
	seen := map[string]bool{}
	for dec.More() {
		name, err := keyOf(dec)
		if err != nil {
			return nil, err
		}
		if seen[name] {
			return nil, fmt.Errorf("label %s is written twice", name)
		}
		seen[name] = true
		var raw json.RawMessage
		if err := dec.Decode(&raw); err != nil {
			return nil, fmt.Errorf("label %s: %w", name, err)
		}
		l := labelIn{name: name, raw: raw}
		if err := parseLabelBody(&l); err != nil {
			return nil, fmt.Errorf("label %s: %w", name, err)
		}
		labels = append(labels, l)
	}
	if err := expectDelim(dec, '}'); err != nil {
		return nil, err
	}
	return labels, nil
}

// parseLabelBody reads a label's entries and its own empty records. Any other key is refused.
func parseLabelBody(l *labelIn) error {
	var body struct {
		Entries []json.RawMessage `json:"entries"`
		Empty   []emptyDoc        `json:"empty"`
	}
	d := json.NewDecoder(bytes.NewReader(l.raw))
	d.DisallowUnknownFields()
	if err := d.Decode(&body); err != nil {
		return err
	}
	l.entries = body.Entries
	for _, raw := range l.entries {
		var k entryKey
		if err := json.Unmarshal(raw, &k); err != nil {
			return err
		}
		l.keys = append(l.keys, k)
	}
	for _, e := range body.Empty {
		if e.Platform == "" {
			return fmt.Errorf("an empty record %s %s has no platform", e.Type, e.Name)
		}
	}
	l.empty = body.Empty
	return nil
}

func expectDelim(dec *json.Decoder, want json.Delim) error {
	t, err := dec.Token()
	if err != nil {
		return err
	}
	if d, ok := t.(json.Delim); !ok || d != want {
		return fmt.Errorf("expected %q, read %v", want, t)
	}
	return nil
}

func keyOf(dec *json.Decoder) (string, error) {
	t, err := dec.Token()
	if err != nil {
		return "", err
	}
	k, ok := t.(string)
	if !ok {
		return "", fmt.Errorf("expected a key, read %v", t)
	}
	return k, nil
}

// moveTopLevelEmpties gives the added manifest's top-level empty records to its one label, with that label's one
// platform. A manifest with top-level empties and any other label count or platform count is refused.
func moveTopLevelEmpties(add *manifestIn) error {
	if len(add.empty) == 0 {
		return nil
	}
	if len(add.labels) != 1 {
		return fmt.Errorf("the added manifest has %d top-level empty records and %d labels: they can move only into one label", len(add.empty), len(add.labels))
	}
	l := &add.labels[0]
	plat := ""
	for _, k := range l.keys {
		switch {
		case plat == "":
			plat = k.Platform
		case k.Platform != plat:
			return fmt.Errorf("the added label %s has two platforms (%s and %s): its top-level empties cannot name one", l.name, plat, k.Platform)
		}
	}
	if plat == "" {
		return fmt.Errorf("the added label %s has no entry, so its top-level empties have no platform", l.name)
	}
	for _, e := range add.empty {
		e.Platform = plat
		l.empty = append(l.empty, e)
	}
	add.empty = nil
	return nil
}

// checkCollisions refuses a label name in both inputs and a record key listed twice.
func checkCollisions(base, add manifestIn) error {
	names := map[string]bool{}
	keys := map[entryKey]string{}
	addKey := func(k entryKey, where string) error {
		if prev, dup := keys[k]; dup {
			return fmt.Errorf("record %s %s %s is listed twice (%s and %s)", k.Platform, k.Type, k.Name, prev, where)
		}
		keys[k] = where
		return nil
	}
	for _, in := range []manifestIn{base, add} {
		for _, l := range in.labels {
			if names[l.name] {
				return fmt.Errorf("label %s is in both manifests", l.name)
			}
			names[l.name] = true
			for _, k := range l.keys {
				if err := addKey(k, l.name); err != nil {
					return err
				}
			}
			for _, e := range l.empty {
				if err := addKey(entryKey{Platform: e.Platform, Type: e.Type, Name: e.Name}, l.name); err != nil {
					return err
				}
			}
		}
	}
	return nil
}

// encodeMerged writes the merged text: the base's labels as their own bytes, then the added labels encoded, in order.
func encodeMerged(base, add manifestIn) ([]byte, []labelReport, error) {
	var b bytes.Buffer
	fmt.Fprintf(&b, `{"%s":"%s","%s":%d,"%s":{`, keySchema, schemaName, keySchemaVer, schemaVersion, keyLabels)
	var reports []labelReport
	first := true
	put := func(name string, value []byte) error {
		if !first {
			b.WriteByte(',')
		}
		first = false
		key, err := json.Marshal(name)
		if err != nil {
			return err
		}
		b.Write(key)
		b.WriteByte(':')
		b.Write(value)
		return nil
	}
	for _, l := range base.labels {
		if err := put(l.name, l.raw); err != nil {
			return nil, nil, err
		}
		reports = append(reports, labelReport{Name: l.name, Entries: len(l.entries), Empty: len(l.empty)})
	}
	for _, l := range add.labels {
		enc, err := encodeLabel(l)
		if err != nil {
			return nil, nil, err
		}
		if err := put(l.name, enc); err != nil {
			return nil, nil, err
		}
		reports = append(reports, labelReport{Name: l.name, Entries: len(l.entries), Empty: len(l.empty)})
	}
	fmt.Fprintf(&b, `},"%s":[]}`, keyEmpty)
	text := b.Bytes()
	if !json.Valid(text) {
		return nil, nil, errors.New("the merged manifest is not valid JSON")
	}
	return text, reports, nil
}

// encodeLabel writes an added label: its entries as they were, and its empty records when it has any.
func encodeLabel(l labelIn) ([]byte, error) {
	entries := l.entries
	if entries == nil {
		entries = []json.RawMessage{}
	}
	ent, err := json.Marshal(entries)
	if err != nil {
		return nil, err
	}
	var b bytes.Buffer
	fmt.Fprintf(&b, `{"%s":%s`, keyEntries, ent)
	if len(l.empty) > 0 {
		em, err := json.Marshal(l.empty)
		if err != nil {
			return nil, err
		}
		fmt.Fprintf(&b, `,"%s":%s`, keyEmpty, em)
	}
	b.WriteByte('}')
	return b.Bytes(), nil
}

// encodeFrame is the manifest as one zstd frame whose header states the content size. The window is the smallest power
// of two that holds the whole text, so the frame is single-segment (cargo-pack's encodeFrame frames a manifest the same way).
func encodeFrame(text []byte) ([]byte, error) {
	window := manifestWindow(len(text))
	enc, err := zstd.NewWriter(nil, zstd.WithEncoderLevel(zstd.SpeedBestCompression), zstd.WithWindowSize(int(window)),
		zstd.WithEncoderCRC(false), zstd.WithSingleSegment(true))
	if err != nil {
		return nil, err
	}
	defer enc.Close()
	return enc.EncodeAll(text, nil), nil
}

// manifestWindow is the smallest power of two the encoder accepts that holds the whole manifest text.
func manifestWindow(textBytes int) uint64 {
	return max(uint64(1)<<bits.Len64(uint64(max(textBytes, 1))-1), zstd.MinWindowSize)
}
