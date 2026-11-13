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
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/klauspost/compress/zstd"
)

const (
	testPrefix      = "v1/allassets/2026-11-12/"
	testBucket      = "fake-bucket"
	testKeyID       = "AKIDFAKE"
	testSecret      = "fake-secret-not-real"
	testRegion      = "auto"
	testDirPerm     = 0o755
	testFilePerm    = 0o644
	testHTTPTimeout = 30 * time.Second
)

// fakeS3 holds objects by key (the part after the bucket) and records each request it takes.
type fakeS3 struct {
	mu      sync.Mutex
	objects map[string][]byte
	puts    int
	heads   int
	// headCode, when set, is the status every HEAD answers (to test a status other than 200 or 404).
	headCode int
	// corrupt, when set, stores a changed byte on every PUT (a mutant for the readback check).
	corrupt bool
	// race, when set, makes HEAD answer 404 while the object is already held, so the PUT meets an existing key (412).
	race bool
	// lastPutHeaders keeps the last PUT's headers for the signing test.
	lastPutHeaders http.Header
	// badSigs counts PUTs whose Authorization does not match the request as received (answered 403, never stored).
	badSigs int
}

// signatureMatches recomputes the SigV4 Authorization from the request as it arrived: its method, its escaped path and
// its signed headers. A signer that signs a different path or header set than it sends is refused.
func signatureMatches(r *http.Request) bool {
	headers := map[string]string{"host": r.Host}
	for _, n := range []string{"If-None-Match", "X-Amz-Content-Sha256", "X-Amz-Date"} {
		if v := r.Header.Get(n); v != "" {
			headers[strings.ToLower(n)] = v
		}
	}
	want := sigV4Authorization(sigInput{
		Method: r.Method, URI: r.URL.EscapedPath(), Headers: headers, PayloadHash: unsignedPayload,
		AmzDate: r.Header.Get("X-Amz-Date"), Region: testRegion, KeyID: testKeyID, Secret: testSecret,
	})
	return r.Header.Get("Authorization") == want
}

func (f *fakeS3) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	f.mu.Lock()
	defer f.mu.Unlock()
	key := strings.TrimPrefix(r.URL.Path, "/")
	// Path-style S3 calls carry the bucket first; the public domain serves the key alone.
	key = strings.TrimPrefix(key, testBucket+"/")
	switch r.Method {
	case http.MethodHead:
		f.heads++
		if f.headCode != 0 {
			w.WriteHeader(f.headCode)
			return
		}
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
		if !signatureMatches(r) {
			f.badSigs++
			w.WriteHeader(http.StatusForbidden)
			return
		}
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
		Bucket:      testBucket,
		Region:      testRegion,
		AccessKeyID: testKeyID,
		SecretKey:   testSecret,
		PublicBase:  srvURL,
		Prefix:      testPrefix,
		Workers:     4,
		HTTP:        &http.Client{Timeout: testHTTPTimeout},
	}
}

// zstdFrame returns b as one zstd frame: the form the manifest and every blob take in the set.
func zstdFrame(t *testing.T, b []byte) []byte {
	t.Helper()
	enc, err := zstd.NewWriter(nil)
	if err != nil {
		t.Fatal(err)
	}
	defer enc.Close()
	return enc.EncodeAll(b, nil)
}

// sha256Hex returns the lower-case hex SHA-256 of b.
func sha256Hex(b []byte) string {
	s := sha256.Sum256(b)
	return hex.EncodeToString(s[:])
}

// testManifest is the content manifest the fixture set holds, as plain JSON (the uploader never reads it).
var testManifest = []byte(`{"schema":"nevr-cargo-manifest","schema_version":1,"labels":{}}`)

// writeSet makes an extracted-set directory: blobs/sha256/<hex> for each body (each one a zstd frame), a plain
// manifest.json the uploader must ignore, and manifest.json.zst, the manifest as a zstd frame. It returns the directory
// and the frame's bytes.
func writeSet(t *testing.T, bodies [][]byte) (string, []byte) {
	t.Helper()
	dir := t.TempDir()
	bdir := filepath.Join(dir, "blobs", "sha256")
	if err := os.MkdirAll(bdir, testDirPerm); err != nil {
		t.Fatal(err)
	}
	for _, b := range bodies {
		frame := zstdFrame(t, b)
		if err := os.WriteFile(filepath.Join(bdir, sha256Hex(frame)), frame, testFilePerm); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.WriteFile(filepath.Join(dir, plainManifestName), testManifest, testFilePerm); err != nil {
		t.Fatal(err)
	}
	frame := zstdFrame(t, testManifest)
	if err := os.WriteFile(filepath.Join(dir, manifestFrameName), frame, testFilePerm); err != nil {
		t.Fatal(err)
	}
	return dir, frame
}

// blobKeyOf is the key a frame's blob takes under the test prefix.
func blobKeyOf(frame []byte) string {
	return testPrefix + blobsDir + sha256Hex(frame)
}

func quietLog() *slog.Logger {
	return slog.New(slog.NewJSONHandler(io.Discard, nil))
}

func TestDryRunCountsObjectsAndBytesWithoutNetwork(t *testing.T) {
	dir, frame := writeSet(t, [][]byte{[]byte("alpha"), []byte("bravo!!"), []byte("charlie")})
	// An endpoint that refuses every connection: a dry run must not dial it.
	cfg := testConfig("http://127.0.0.1:1")
	p, err := buildPlan(dir, cfg.Prefix)
	if err != nil {
		t.Fatalf("buildPlan: %v", err)
	}
	var blobBytes int64
	entries, _ := os.ReadDir(filepath.Join(dir, "blobs", "sha256"))
	for _, e := range entries {
		info, _ := e.Info()
		blobBytes += info.Size()
	}
	wantBytes := blobBytes + 2*int64(len(frame))
	if p.Blobs != 3 || len(p.Objects) != 5 {
		t.Fatalf("plan: blobs=%d objects=%d, want 3 blobs and 5 objects (3 blobs, 2 manifest keys)", p.Blobs, len(p.Objects))
	}
	if p.Bytes != wantBytes {
		t.Fatalf("plan bytes=%d, want %d", p.Bytes, wantBytes)
	}
	sum := dryRunSummary(p)
	if !sum.Dry || sum.Objects != 5 || sum.Bytes != wantBytes {
		t.Fatalf("dry summary = %+v", sum)
	}
}

func TestUploadPutsMissingSkipsExistingAndVerifiesByReadback(t *testing.T) {
	f, srv := newFake(t)
	bodies := [][]byte{[]byte("one"), []byte("two-two"), []byte("three-three-3")}
	dir, frame := writeSet(t, bodies)
	// The first blob is already on the bucket: it must be skipped, not put again.
	first := zstdFrame(t, bodies[0])
	f.objects[blobKeyOf(first)] = first

	cfg := testConfig(srv.URL)
	p, err := buildPlan(dir, cfg.Prefix)
	if err != nil {
		t.Fatalf("buildPlan: %v", err)
	}
	sum, err := runPublish(context.Background(), cfg, p, quietLog())
	if err != nil {
		t.Fatalf("runPublish: %v", err)
	}
	if sum.Skipped != 1 || sum.Uploaded != 4 || sum.Verified != 4 {
		t.Fatalf("summary = %+v, want skipped=1 uploaded=4 verified=4", sum)
	}
	if f.puts != 4 {
		t.Fatalf("PUT count = %d, want 4 (two blobs and the two manifest keys)", f.puts)
	}
	for _, b := range bodies[1:] {
		fr := zstdFrame(t, b)
		if !bytes.Equal(f.objects[blobKeyOf(fr)], fr) {
			t.Fatalf("blob not stored at %s", blobKeyOf(fr))
		}
	}
	if !bytes.Equal(f.objects[testPrefix+manifestFrameName], frame) {
		t.Fatalf("manifest frame not stored at %s", testPrefix+manifestFrameName)
	}
	if !bytes.Equal(f.objects[manifestsKeyDir+sha256Hex(frame)+manifestFrameExt], frame) {
		t.Fatalf("manifest frame not stored under %s", manifestsKeyDir)
	}
	for k := range f.objects {
		if k == liveManifestKey || k == testPrefix+plainManifestName || strings.HasSuffix(k, "/"+plainManifestName) {
			t.Fatalf("a plain or live manifest key was written: %s", k)
		}
	}
}

func TestNeverOverwritesAnObjectThatAppearsBeforeThePut(t *testing.T) {
	f, srv := newFake(t)
	dir, _ := writeSet(t, [][]byte{[]byte("the blob")})
	frame := zstdFrame(t, []byte("the blob"))
	key := blobKeyOf(frame)
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
	if err := os.MkdirAll(bdir, testDirPerm); err != nil {
		t.Fatal(err)
	}
	name := sha256Hex(zstdFrame(t, []byte("what the name says")))
	if err := os.WriteFile(filepath.Join(bdir, name), zstdFrame(t, []byte("what the file holds")), testFilePerm); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, manifestFrameName), zstdFrame(t, testManifest), testFilePerm); err != nil {
		t.Fatal(err)
	}
	if _, err := buildPlan(dir, testConfig(srv.URL).Prefix); err == nil || !strings.Contains(err.Error(), name) {
		t.Fatalf("buildPlan error = %v; want a refusal naming %s", err, name)
	}
	if f.puts != 0 {
		t.Fatalf("PUT count = %d before refusal, want 0", f.puts)
	}
}

func TestBlobWithoutZstdMagicIsRefusedBeforeAnyPut(t *testing.T) {
	f, srv := newFake(t)
	dir, _ := writeSet(t, nil)
	// A plain (uncompressed) blob whose bytes hash to its name: the name and hash checks pass, so only the magic refuses it.
	plain := []byte("plain uncompressed blob, never on the CDN")
	name := sha256Hex(plain)
	if err := os.WriteFile(filepath.Join(dir, "blobs", "sha256", name), plain, testFilePerm); err != nil {
		t.Fatal(err)
	}
	_, err := buildPlan(dir, testConfig(srv.URL).Prefix)
	if err == nil {
		t.Fatal("buildPlan accepted a blob that is not a zstd frame")
	}
	if !strings.Contains(err.Error(), name) || !strings.Contains(err.Error(), "zstd") {
		t.Fatalf("refusal does not name the blob and the zstd magic: %v", err)
	}
	if f.puts != 0 {
		t.Fatalf("PUT count = %d before refusal, want 0", f.puts)
	}
}

func TestPlainManifestIsRefusedAndNeverPlanned(t *testing.T) {
	_, srv := newFake(t)
	dir, _ := writeSet(t, [][]byte{[]byte("one")})
	// Only the plain manifest: the uploader refuses, and names why.
	plainOnly := t.TempDir()
	if err := os.MkdirAll(filepath.Join(plainOnly, "blobs", "sha256"), testDirPerm); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(plainOnly, plainManifestName), testManifest, testFilePerm); err != nil {
		t.Fatal(err)
	}
	if _, err := buildPlan(plainOnly, testConfig(srv.URL).Prefix); err == nil || !strings.Contains(err.Error(), "never uploaded") {
		t.Fatalf("plain-only set: buildPlan error = %v; want a plain-manifest refusal", err)
	}
	// With both files present, the plan carries the frame keys and no plain manifest key.
	p, err := buildPlan(dir, testConfig(srv.URL).Prefix)
	if err != nil {
		t.Fatalf("buildPlan: %v", err)
	}
	for _, o := range p.Objects {
		if strings.HasSuffix(o.Key, plainManifestName) || o.Key == liveManifestKey {
			t.Fatalf("plan names a plain or live manifest key: %s", o.Key)
		}
		if strings.HasSuffix(o.Key, manifestFrameName) || strings.HasSuffix(o.Key, manifestFrameExt) {
			if err := checkZstd(o.Local); err != nil {
				t.Fatalf("manifest key %s points at a non-zstd file: %v", o.Key, err)
			}
		}
	}
}

func TestCheckPrefixRefusesEscapesAndRelativeForms(t *testing.T) {
	for _, bad := range []string{"", "v1/allassets", "/v1/allassets/", "v1/../x/", "../x/", "v1//../x/"} {
		if err := checkPrefix(bad); err == nil {
			t.Errorf("checkPrefix(%q) accepted a prefix that is not a relative folder path", bad)
		}
	}
	if err := checkPrefix(testPrefix); err != nil {
		t.Fatalf("checkPrefix(%q) refused the test prefix: %v", testPrefix, err)
	}
	if _, err := buildPlan(t.TempDir(), "v1/allassets"); err == nil {
		t.Fatal("buildPlan accepted a prefix without a trailing slash")
	}
}

func TestHeadAnswerOtherThan200Or404FailsWithoutPut(t *testing.T) {
	f, srv := newFake(t)
	f.headCode = http.StatusInternalServerError
	dir, _ := writeSet(t, [][]byte{[]byte("one")})
	cfg := testConfig(srv.URL)
	p, err := buildPlan(dir, cfg.Prefix)
	if err != nil {
		t.Fatalf("buildPlan: %v", err)
	}
	if _, err := runPublish(context.Background(), cfg, p, quietLog()); err == nil || !strings.Contains(err.Error(), "500") {
		t.Fatalf("runPublish error = %v; want the HEAD status named", err)
	}
	if f.puts != 0 {
		t.Fatalf("PUT count = %d after a HEAD error, want 0", f.puts)
	}
}

// TestSigV4KnownAnswer checks sigV4Authorization against the worked GET example in the AWS S3 documentation
// ("Examples of the complete Signature Version 4 signing process"): fixed keys, date and headers, one expected signature.
func TestSigV4KnownAnswer(t *testing.T) {
	got := sigV4Authorization(sigInput{
		Method: "GET",
		URI:    "/test.txt",
		Headers: map[string]string{
			"host":                 "examplebucket.s3.amazonaws.com",
			"range":                "bytes=0-9",
			"x-amz-content-sha256": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
			"x-amz-date":           "20130524T000000Z",
		},
		PayloadHash: "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
		AmzDate:     "20130524T000000Z",
		Region:      "us-east-1",
		KeyID:       "AKIAIOSFODNN7EXAMPLE",
		Secret:      "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY",
	})
	const want = "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request, " +
		"SignedHeaders=host;range;x-amz-content-sha256;x-amz-date, " +
		"Signature=f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41"
	if got != want {
		t.Fatalf("Authorization =\n  %s\nwant\n  %s", got, want)
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
	if f.badSigs != 0 {
		t.Fatalf("%d PUT(s) carried a signature that does not match the request as sent", f.badSigs)
	}
	h := f.lastPutHeaders
	auth := h.Get("Authorization")
	if !strings.HasPrefix(auth, "AWS4-HMAC-SHA256 Credential="+testKeyID+"/") ||
		!strings.Contains(auth, "/"+testRegion+"/s3/aws4_request") ||
		!strings.Contains(auth, "SignedHeaders=") || !strings.Contains(auth, "if-none-match") ||
		!strings.Contains(auth, "Signature=") {
		t.Fatalf("Authorization = %q", auth)
	}
	if h.Get("If-None-Match") != "*" {
		t.Fatalf("If-None-Match = %q, want *", h.Get("If-None-Match"))
	}
	if h.Get("X-Amz-Content-Sha256") != unsignedPayload || h.Get("X-Amz-Date") == "" {
		t.Fatalf("missing SigV4 headers: %v", h)
	}
}

func TestMissingCredentialNamesItsVariable(t *testing.T) {
	env := map[string]string{
		"NEVR_CARGO_S3_ENDPOINT":      "https://example.invalid",
		"NEVR_CARGO_S3_BUCKET":        "b",
		"NEVR_CARGO_S3_ACCESS_KEY_ID": "id",
		"NEVR_CARGO_PUBLIC_BASE":      "https://cargo.echo.taxi",
		"NEVR_CARGO_PREFIX":           testPrefix,
	}
	_, err := loadConfig(func(k string) string { return env[k] })
	if err == nil || !strings.Contains(err.Error(), "NEVR_CARGO_S3_SECRET_ACCESS_KEY") {
		t.Fatalf("loadConfig error = %v; want it to name NEVR_CARGO_S3_SECRET_ACCESS_KEY", err)
	}
}

// buildBinary compiles this package's main into a temporary file, so a test runs the binary the way an operator does.
func buildBinary(t *testing.T) string {
	t.Helper()
	out := filepath.Join(t.TempDir(), "cargo-publish")
	cmd := exec.Command("go", "build", "-o", out, ".")
	if msg, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("go build: %v\n%s", err, msg)
	}
	return out
}

// TestBinaryDryRunPrintsTheCountsAndSendsNothing runs the built binary with -dry-run against an endpoint that refuses
// connections, and checks the stdout summary and the stderr record.
func TestBinaryDryRunPrintsTheCountsAndSendsNothing(t *testing.T) {
	bin := buildBinary(t)
	dir, _ := writeSet(t, [][]byte{[]byte("alpha"), []byte("bravo")})
	cmd := exec.Command(bin, "-dir", dir, "-dry-run")
	cmd.Env = []string{"NEVR_CARGO_PREFIX=" + testPrefix, "NEVR_CARGO_S3_ENDPOINT=http://127.0.0.1:1"}
	var stdout, stderr bytes.Buffer
	cmd.Stdout, cmd.Stderr = &stdout, &stderr
	if err := cmd.Run(); err != nil {
		t.Fatalf("dry run exited with %v\nstderr:\n%s", err, stderr.String())
	}
	if !strings.Contains(stdout.String(), `"dry_run":true`) || !strings.Contains(stdout.String(), `"objects":4`) {
		t.Fatalf("stdout summary = %s; want dry_run true and 4 objects (2 blobs, 2 manifest keys)", stdout.String())
	}
	if !strings.Contains(stderr.String(), `"msg":"dry run"`) {
		t.Fatalf("stderr has no dry-run record: %s", stderr.String())
	}
}

// TestBinaryUploadLogsEachUploadedKey runs the built binary against the fake S3 and checks the exit code, the summary
// and an Info record per uploaded key, as the operator's log shows it.
func TestBinaryUploadLogsEachUploadedKey(t *testing.T) {
	bin := buildBinary(t)
	f, srv := newFake(t)
	dir, frame := writeSet(t, [][]byte{[]byte("log me")})
	cmd := exec.Command(bin, "-dir", dir, "-workers", "2")
	cmd.Env = []string{
		"NEVR_CARGO_S3_ENDPOINT=" + srv.URL,
		"NEVR_CARGO_S3_BUCKET=" + testBucket,
		"NEVR_CARGO_S3_ACCESS_KEY_ID=" + testKeyID,
		"NEVR_CARGO_S3_SECRET_ACCESS_KEY=" + testSecret,
		"NEVR_CARGO_PUBLIC_BASE=" + srv.URL,
		"NEVR_CARGO_PREFIX=" + testPrefix,
	}
	var stdout, stderr bytes.Buffer
	cmd.Stdout, cmd.Stderr = &stdout, &stderr
	if err := cmd.Run(); err != nil {
		t.Fatalf("upload exited with %v\nstderr:\n%s", err, stderr.String())
	}
	if f.puts != 3 {
		t.Fatalf("PUT count = %d, want 3 (one blob, two manifest keys)", f.puts)
	}
	manifestKey := manifestsKeyDir + sha256Hex(frame) + manifestFrameExt
	for _, key := range []string{blobKeyOf(zstdFrame(t, []byte("log me"))), testPrefix + manifestFrameName, manifestKey} {
		if !strings.Contains(stderr.String(), `"msg":"object uploaded"`) || !strings.Contains(stderr.String(), key) {
			t.Fatalf("stderr has no uploaded record for %s:\n%s", key, stderr.String())
		}
	}
	if !strings.Contains(stdout.String(), `"uploaded":3`) {
		t.Fatalf("stdout summary = %s; want uploaded 3", stdout.String())
	}
}
