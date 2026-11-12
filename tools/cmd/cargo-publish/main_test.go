package main

// P2 (cdn-all-assets): cargo-publish puts an extracted object set and its content manifest into an S3 bucket under one
// prefix. These tests run it against a fake S3 served by httptest: nothing reaches the network, and no key is read.

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"
)

// fakeS3 holds objects by key (the part after the bucket) and records each request it takes.
type fakeS3 struct {
	mu      sync.Mutex
	objects map[string][]byte
	puts    int
	heads   int
	// corrupt, when set, stores a changed byte on every PUT (a mutant for the readback check).
	corrupt bool
	// race, when set, makes HEAD answer 404 and the PUT answer 412 over bytes it already holds (an overwrite attempt).
	race bool
	// lastPutAuth and lastPutHeaders keep the last PUT's signing headers for the SigV4 test.
	lastPutHeaders http.Header
}

func (f *fakeS3) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	f.mu.Lock()
	defer f.mu.Unlock()
	key := strings.TrimPrefix(r.URL.Path, "/")
	// Path-style S3 calls carry the bucket first; the public domain serves the key alone.
	key = strings.TrimPrefix(key, "fake-bucket/")
	switch r.Method {
	case http.MethodHead:
		f.heads++
		if _, ok := f.objects[key]; ok && !f.race {
			w.WriteHeader(http.StatusOK)
			return
		}
		w.WriteHeader(http.StatusNotFound)
	case http.MethodGet:
		b, ok := f.objects[key]
		if !ok {
			w.WriteHeader(http.StatusNotFound)
			return
		}
		_, _ = w.Write(b)
	case http.MethodPut:
		f.puts++
		f.lastPutHeaders = r.Header.Clone()
		body, _ := io.ReadAll(r.Body)
		if _, exists := f.objects[key]; exists && r.Header.Get("If-None-Match") == "*" {
			w.WriteHeader(http.StatusPreconditionFailed)
			return
		}
		if f.corrupt && len(body) > 0 {
			body[0] ^= 0xff
		}
		f.objects[key] = body
		w.WriteHeader(http.StatusOK)
	default:
		w.WriteHeader(http.StatusMethodNotAllowed)
	}
}

func newFake(t *testing.T) (*fakeS3, *httptest.Server) {
	t.Helper()
	f := &fakeS3{objects: map[string][]byte{}}
	srv := httptest.NewServer(f)
	t.Cleanup(srv.Close)
	return f, srv
}

func testConfig(srvURL string) config {
	return config{
		Endpoint:    srvURL,
		Bucket:      "fake-bucket",
		Region:      "auto",
		AccessKeyID: "AKIDFAKE",
		SecretKey:   "fake-secret-not-real",
		PublicBase:  srvURL,
		Prefix:      "v1/allassets/2026-11-12/",
		Workers:     4,
		HTTP:        &http.Client{Timeout: 30 * time.Second},
	}
}

func sha256hex(b []byte) string {
	s := sha256.Sum256(b)
	return hex.EncodeToString(s[:])
}

// writeSet makes an extracted-set directory: blobs/sha256/<hex> for each body, and a manifest.json.
func writeSet(t *testing.T, bodies [][]byte) (string, [][]byte) {
	t.Helper()
	dir := t.TempDir()
	bdir := filepath.Join(dir, "blobs", "sha256")
	if err := os.MkdirAll(bdir, 0o755); err != nil {
		t.Fatal(err)
	}
	for _, b := range bodies {
		if err := os.WriteFile(filepath.Join(bdir, sha256hex(b)), b, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	manifest := []byte(`{"schema":"nevr-cargo-manifest","schema_version":1,"labels":{}}`)
	if err := os.WriteFile(filepath.Join(dir, "manifest.json"), manifest, 0o644); err != nil {
		t.Fatal(err)
	}
	return dir, append(bodies, manifest)
}

func quietLog() *slog.Logger {
	return slog.New(slog.NewJSONHandler(io.Discard, nil))
}

func TestDryRunCountsObjectsAndBytesWithoutNetwork(t *testing.T) {
	dir, all := writeSet(t, [][]byte{[]byte("alpha"), []byte("bravo!!"), []byte("charlie")})
	// An endpoint that refuses every connection: a dry run must not dial it.
	cfg := testConfig("http://127.0.0.1:1")
	p, err := buildPlan(dir, cfg.Prefix)
	if err != nil {
		t.Fatalf("buildPlan: %v", err)
	}
	var wantBytes int64
	for _, b := range all {
		wantBytes += int64(len(b))
	}
	if p.Blobs != 3 || len(p.Objects) != 4 {
		t.Fatalf("plan: blobs=%d objects=%d, want 3 and 4", p.Blobs, len(p.Objects))
	}
	if p.Bytes != wantBytes {
		t.Fatalf("plan bytes=%d, want %d", p.Bytes, wantBytes)
	}
	sum := dryRunSummary(p)
	if !sum.Dry || sum.Objects != 4 || sum.Bytes != wantBytes {
		t.Fatalf("dry summary = %+v", sum)
	}
}

func TestUploadPutsMissingSkipsExistingAndVerifiesByReadback(t *testing.T) {
	f, srv := newFake(t)
	bodies := [][]byte{[]byte("one"), []byte("two-two"), []byte("three-three-3")}
	dir, _ := writeSet(t, bodies)
	// The first blob is already on the bucket: it must be skipped, not put again.
	f.objects["v1/allassets/2026-11-12/blobs/sha256/"+sha256hex(bodies[0])] = bodies[0]

	cfg := testConfig(srv.URL)
	p, err := buildPlan(dir, cfg.Prefix)
	if err != nil {
		t.Fatalf("buildPlan: %v", err)
	}
	sum, err := runPublish(context.Background(), cfg, p, quietLog())
	if err != nil {
		t.Fatalf("runPublish: %v", err)
	}
	if sum.Skipped != 1 || sum.Uploaded != 3 || sum.Verified != 3 {
		t.Fatalf("summary = %+v, want skipped=1 uploaded=3 verified=3", sum)
	}
	if f.puts != 3 {
		t.Fatalf("PUT count = %d, want 3 (two blobs and the manifest)", f.puts)
	}
	for i, b := range bodies[1:] {
		key := "v1/allassets/2026-11-12/blobs/sha256/" + sha256hex(b)
		if !bytes.Equal(f.objects[key], b) {
			t.Fatalf("object %d not stored under %s", i+1, key)
		}
	}
	if _, ok := f.objects["v1/allassets/2026-11-12/manifest.json"]; !ok {
		t.Fatal("manifest.json not stored under the prefix")
	}
	if _, ok := f.objects["v1/manifest.json"]; ok {
		t.Fatal("the live manifest key was written")
	}
}

func TestNeverOverwritesAnObjectThatAppearsBeforeThePut(t *testing.T) {
	f, srv := newFake(t)
	body := []byte("the blob")
	dir, _ := writeSet(t, [][]byte{body})
	key := "v1/allassets/2026-11-12/blobs/sha256/" + sha256hex(body)
	// HEAD says absent, but the object is already there with other bytes: the conditional PUT must refuse.
	f.objects[key] = []byte("someone else's bytes")
	f.race = true

	cfg := testConfig(srv.URL)
	p, err := buildPlan(dir, cfg.Prefix)
	if err != nil {
		t.Fatalf("buildPlan: %v", err)
	}
	if _, err := runPublish(context.Background(), cfg, p, quietLog()); err == nil {
		t.Fatal("runPublish succeeded over an existing object; want a refusal")
	}
	if !bytes.Equal(f.objects[key], []byte("someone else's bytes")) {
		t.Fatal("existing object was overwritten")
	}
}

func TestReadbackMismatchFails(t *testing.T) {
	f, srv := newFake(t)
	dir, _ := writeSet(t, [][]byte{[]byte("payload")})
	f.corrupt = true

	cfg := testConfig(srv.URL)
	p, err := buildPlan(dir, cfg.Prefix)
	if err != nil {
		t.Fatalf("buildPlan: %v", err)
	}
	if _, err := runPublish(context.Background(), cfg, p, quietLog()); err == nil {
		t.Fatal("runPublish succeeded although the stored bytes differ; want a readback failure")
	} else if !strings.Contains(err.Error(), "readback") {
		t.Fatalf("error does not name the readback: %v", err)
	}
}

func TestBlobWhoseBytesDoNotMatchItsNameIsRefusedBeforeAnyPut(t *testing.T) {
	f, srv := newFake(t)
	dir := t.TempDir()
	bdir := filepath.Join(dir, "blobs", "sha256")
	if err := os.MkdirAll(bdir, 0o755); err != nil {
		t.Fatal(err)
	}
	name := sha256hex([]byte("what the name says"))
	if err := os.WriteFile(filepath.Join(bdir, name), []byte("what the file holds"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "manifest.json"), []byte("{}"), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := buildPlan(dir, testConfig(srv.URL).Prefix); err == nil || !strings.Contains(err.Error(), name) {
		t.Fatalf("buildPlan error = %v; want a refusal naming %s", err, name)
	}
	if f.puts != 0 {
		t.Fatalf("PUT count = %d before refusal, want 0", f.puts)
	}
}

func TestSignedPutCarriesSigV4AndNoOverwriteHeaders(t *testing.T) {
	f, srv := newFake(t)
	dir, _ := writeSet(t, [][]byte{[]byte("signed")})
	cfg := testConfig(srv.URL)
	p, err := buildPlan(dir, cfg.Prefix)
	if err != nil {
		t.Fatalf("buildPlan: %v", err)
	}
	if _, err := runPublish(context.Background(), cfg, p, quietLog()); err != nil {
		t.Fatalf("runPublish: %v", err)
	}
	h := f.lastPutHeaders
	auth := h.Get("Authorization")
	if !strings.HasPrefix(auth, "AWS4-HMAC-SHA256 Credential=AKIDFAKE/") ||
		!strings.Contains(auth, "/auto/s3/aws4_request") ||
		!strings.Contains(auth, "SignedHeaders=") || !strings.Contains(auth, "if-none-match") ||
		!strings.Contains(auth, "Signature=") {
		t.Fatalf("Authorization = %q", auth)
	}
	if h.Get("If-None-Match") != "*" {
		t.Fatalf("If-None-Match = %q, want *", h.Get("If-None-Match"))
	}
	if h.Get("X-Amz-Content-Sha256") != "UNSIGNED-PAYLOAD" || h.Get("X-Amz-Date") == "" {
		t.Fatalf("missing SigV4 headers: %v", h)
	}
}

func TestMissingCredentialNamesItsVariable(t *testing.T) {
	env := map[string]string{
		"NEVR_CARGO_S3_ENDPOINT":      "https://example.invalid",
		"NEVR_CARGO_S3_BUCKET":        "b",
		"NEVR_CARGO_S3_ACCESS_KEY_ID": "id",
		"NEVR_CARGO_PUBLIC_BASE":      "https://cargo.echo.taxi",
		"NEVR_CARGO_PREFIX":           "v1/allassets/2026-11-12/",
	}
	_, err := loadConfig(func(k string) string { return env[k] })
	if err == nil || !strings.Contains(err.Error(), "NEVR_CARGO_S3_SECRET_ACCESS_KEY") {
		t.Fatalf("loadConfig error = %v; want it to name NEVR_CARGO_S3_SECRET_ACCESS_KEY", err)
	}
}
