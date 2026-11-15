package main

// P2 (cdn-all-assets): cargo-publish puts an extracted object set and its content manifest into an S3 bucket under one
// prefix. These tests run it against a fake S3 served by httptest: nothing reaches the network, and no key is read.

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
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

// fakeS3 holds objects by key (the part after the bucket) and records each request it takes. A path that starts with the
// bucket is an S3 API call and must carry a SigV4 signature that matches it; any other path is the public domain, which
// serves the bucket's objects through an edge cache (edge) that may hold older copies.
type fakeS3 struct {
	mu      sync.Mutex
	objects map[string][]byte
	puts    int
	heads   int
	// headCode, when set, is the status every S3 HEAD answers (to test a status other than 200 or 404).
	headCode int
	// corrupt, when set, stores a changed byte on every PUT (a mutant for the readback check).
	corrupt bool
	// race, when set, makes S3 HEAD answer 404 while the object is already held, so the PUT meets an existing key (412).
	race bool
	// lastPutHeaders keeps the last PUT's headers for the signing test; putHeaders keeps every PUT's, by key.
	lastPutHeaders http.Header
	putHeaders     map[string]http.Header
	// putOrder lists the keys of the PUTs in the order they were taken.
	putOrder []string
	// badSigs counts S3 calls whose Authorization does not match the request as received (answered 403, never served).
	badSigs int
	// ifMatch lists the If-Match value of each PUT that sent one.
	ifMatch []string
	// noETag makes S3 GET answer without an ETag.
	noETag bool
	// corruptKey, when set with corrupt, limits the corruption to the PUT of that key.
	corruptKey string
	// dropKey, when set, makes a PUT of that key answer 200 and store nothing: the bucket still holds the old bytes.
	dropKey string
	// swapKey, when set, makes the object at that key take the bytes swapTo right after its first S3 GET is served: a
	// second writer that lands between a check and a conditional PUT.
	swapKey string
	swapTo  []byte
	// edge is the public domain's cache: a key in it is served from it (cf-cache-status HIT), whatever the bucket holds.
	edge map[string]edgeCopy
	// publicReqs counts the requests the public domain took.
	publicReqs int
}

// edgeCopy is what the public domain's edge cache holds for a key: a body with status 200, or a cached 404 (body nil).
type edgeCopy struct {
	status int
	body   []byte
}

// testEdgeAge is the Age the fake edge sends with a cached copy (the age the real edge sent with its stale copy).
const testEdgeAge = "126"

// etagOf is the ETag the fake serves for an object's bytes: the quoted SHA-256.
func etagOf(b []byte) string { return `"` + sha256Hex(b) + `"` }

// signedHeadersOf returns the SignedHeaders list of a SigV4 Authorization value, or nil.
func signedHeadersOf(auth string) []string {
	const field = "SignedHeaders="
	i := strings.Index(auth, field)
	if i < 0 {
		return nil
	}
	rest := auth[i+len(field):]
	if j := strings.Index(rest, ","); j >= 0 {
		rest = rest[:j]
	}
	return strings.Split(rest, ";")
}

// signatureMatches recomputes the SigV4 Authorization as the bucket does: from the request as it arrived, its method, its
// escaped path and every header its SignedHeaders names, read from the request. host and both x-amz headers must be among
// them. A signer that signs a different path, header set or header value than it sends is refused.
func signatureMatches(r *http.Request) bool {
	names := signedHeadersOf(r.Header.Get("Authorization"))
	headers := map[string]string{}
	for _, n := range names {
		if n == "host" {
			headers[n] = r.Host
			continue
		}
		headers[n] = r.Header.Get(n)
	}
	for _, need := range []string{"host", "x-amz-content-sha256", "x-amz-date"} {
		if _, ok := headers[need]; !ok {
			return false
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
	path := strings.TrimPrefix(r.URL.Path, "/")
	if !strings.HasPrefix(path, testBucket+"/") {
		f.servePublic(w, r, path)
		return
	}
	key := strings.TrimPrefix(path, testBucket+"/")
	if !signatureMatches(r) {
		f.badSigs++
		w.WriteHeader(http.StatusForbidden)
		return
	}
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
		if !f.noETag {
			w.Header().Set("ETag", etagOf(b))
		}
		_, _ = w.Write(b)
		if key == f.swapKey && f.swapTo != nil {
			f.objects[key] = f.swapTo
			f.swapTo = nil
		}
	case http.MethodPut:
		f.puts++
		f.putOrder = append(f.putOrder, key)
		f.lastPutHeaders = r.Header.Clone()
		if f.putHeaders == nil {
			f.putHeaders = map[string]http.Header{}
		}
		f.putHeaders[key] = r.Header.Clone()
		body, _ := io.ReadAll(r.Body)
		if im := r.Header.Get("If-Match"); im != "" {
			f.ifMatch = append(f.ifMatch, im)
			if cur, ok := f.objects[key]; !ok || etagOf(cur) != im {
				w.WriteHeader(http.StatusPreconditionFailed)
				return
			}
		}
		if _, exists := f.objects[key]; exists && r.Header.Get("If-None-Match") == "*" {
			w.WriteHeader(http.StatusPreconditionFailed)
			return
		}
		if f.corrupt && len(body) > 0 && (f.corruptKey == "" || key == f.corruptKey) {
			body[0] ^= 0xff
		}
		if key != f.dropKey {
			f.objects[key] = body
		}
		w.WriteHeader(http.StatusOK)
	default:
		w.WriteHeader(http.StatusMethodNotAllowed)
	}
}

// servePublic is the public domain: the edge's copy when it holds one (HIT, with an Age), else the bucket's (MISS).
func (f *fakeS3) servePublic(w http.ResponseWriter, r *http.Request, key string) {
	f.publicReqs++
	if r.Method != http.MethodGet && r.Method != http.MethodHead {
		w.WriteHeader(http.StatusMethodNotAllowed)
		return
	}
	status, body := http.StatusNotFound, []byte(nil)
	if c, ok := f.edge[key]; ok {
		w.Header().Set(cacheStatusHeader, "HIT")
		w.Header().Set(ageHeader, testEdgeAge)
		status, body = c.status, c.body
	} else {
		w.Header().Set(cacheStatusHeader, "MISS")
		if b, ok := f.objects[key]; ok {
			status, body = http.StatusOK, b
		}
	}
	if status == http.StatusOK {
		w.Header().Set("ETag", etagOf(body))
	}
	w.WriteHeader(status)
	if r.Method == http.MethodGet {
		_, _ = w.Write(body)
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

func TestStagingPrefixKeepsEveryKeyInsideThePrefix(t *testing.T) {
	const staging = "v1-staging/"
	dir, frame := writeSet(t, [][]byte{[]byte("alpha")})
	p, err := buildPlan(dir, staging)
	if err != nil {
		t.Fatalf("buildPlan: %v", err)
	}
	for _, o := range p.Objects {
		if !strings.HasPrefix(o.Key, staging) {
			t.Errorf("key %s is outside %s", o.Key, staging)
		}
	}
	want := staging + "manifests/" + sha256Hex(frame) + manifestFrameExt
	found := false
	for _, o := range p.Objects {
		found = found || o.Key == want
	}
	if !found {
		t.Errorf("no key %s in the plan", want)
	}
}

func TestAPresentManifestKeyWithOtherContentIsAnErrorNotASkip(t *testing.T) {
	dir, _ := writeSet(t, [][]byte{[]byte("alpha")})
	f, srv := newFake(t)
	f.objects[testPrefix+manifestFrameName] = []byte("another set's manifest")
	cfg := testConfig(srv.URL)
	p, err := buildPlan(dir, cfg.Prefix)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := runPublish(context.Background(), cfg, p, quietLog()); err == nil || !strings.Contains(err.Error(), "present with other content") {
		t.Fatalf("err = %v, want present with other content", err)
	}
	if string(f.objects[testPrefix+manifestFrameName]) != "another set's manifest" {
		t.Error("the present object was overwritten")
	}
}

func TestLivePrefixPutsBlobsAtTheRootAndManifestsUnderV1(t *testing.T) {
	dir, frame := writeSet(t, [][]byte{[]byte("alpha")})
	p, err := buildPlan(dir, livePrefix)
	if err != nil {
		t.Fatal(err)
	}
	want := map[string]bool{
		blobsDir + sha256Hex(zstdFrame(t, []byte("alpha"))):   true,
		"v1/manifest.json.zst":                                true,
		manifestsKeyDir + sha256Hex(frame) + manifestFrameExt: true,
	}
	for _, o := range p.Objects {
		if !want[o.Key] {
			t.Errorf("unexpected key %s", o.Key)
		}
		delete(want, o.Key)
	}
	for k := range want {
		t.Errorf("missing key %s", k)
	}
}

func TestManifestsAreSentOnlyAfterEveryBlob(t *testing.T) {
	const blobCount = 3
	dir, frame := writeSet(t, [][]byte{[]byte("alpha"), []byte("bravo!!"), []byte("charlie")})
	f, srv := newFake(t)
	cfg := testConfig(srv.URL)
	cfg.Prefix = livePrefix
	p, err := buildPlan(dir, cfg.Prefix)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := runPublish(context.Background(), cfg, p, quietLog()); err != nil {
		t.Fatal(err)
	}
	order := f.putOrder
	if len(order) != blobCount+2 || string(f.objects["v1/manifest.json.zst"]) != string(frame) {
		t.Fatalf("puts = %v", order)
	}
	if order[blobCount] != manifestsKeyDir+sha256Hex(frame)+manifestFrameExt || order[blobCount+1] != "v1/manifest.json.zst" {
		t.Errorf("the last two puts are %v, want the by-hash manifest then the current one", order[blobCount:])
	}
	for _, k := range order[:blobCount] {
		if !strings.HasPrefix(k, blobsDir) {
			t.Errorf("put %s before the manifests is not a blob", k)
		}
	}
}

func TestAFailedBlobStopsTheManifests(t *testing.T) {
	dir, _ := writeSet(t, [][]byte{[]byte("alpha")})
	f, srv := newFake(t)
	f.corrupt = true
	cfg := testConfig(srv.URL)
	cfg.Prefix = livePrefix
	p, err := buildPlan(dir, cfg.Prefix)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := runPublish(context.Background(), cfg, p, quietLog()); err == nil {
		t.Fatal("a corrupt blob readback was accepted")
	}
	for _, k := range f.putOrder {
		if !strings.HasPrefix(k, blobsDir) {
			t.Errorf("manifest key %s was put after a blob failed", k)
		}
	}
}

// replaceSet makes a set whose manifest frame is the bytes given (blobs are not needed for these checks).
func replaceSet(t *testing.T, manifestFrame []byte) string {
	t.Helper()
	dir := t.TempDir()
	if err := os.MkdirAll(filepath.Join(dir, "blobs", "sha256"), testDirPerm); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, manifestFrameName), manifestFrame, testFilePerm); err != nil {
		t.Fatal(err)
	}
	return dir
}

// liveFixture puts the live manifest at old and its recovery copy under the by-hash key, as a publish leaves them.
func liveFixture(t *testing.T, f *fakeS3, old []byte) {
	t.Helper()
	f.objects[livePrefix+manifestFrameName] = old
	f.objects[manifestsKeyDir+sha256Hex(old)+manifestFrameExt] = old
}

func replaceCfg(srvURL, expected string) config {
	cfg := testConfig(srvURL)
	cfg.Prefix = livePrefix
	cfg.ReplaceLive = expected
	return cfg
}

func TestReplaceLiveReplacesTheLiveManifestWithIfMatchAndReadsItBack(t *testing.T) {
	f, srv := newFake(t)
	oldFrame := zstdFrame(t, []byte("the manifest being replaced"))
	liveFixture(t, f, oldFrame)
	dir := replaceSet(t, zstdFrame(t, []byte("the new manifest")))
	newFrame := readFileT(t, filepath.Join(dir, manifestFrameName))
	cfg := replaceCfg(srv.URL, sha256Hex(oldFrame))
	p, err := buildPlan(dir, cfg.Prefix)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := runPublish(context.Background(), cfg, p, quietLog()); err != nil {
		t.Fatalf("runPublish: %v", err)
	}
	if !bytes.Equal(f.objects[livePrefix+manifestFrameName], newFrame) {
		t.Fatal("the live manifest was not replaced")
	}
	if len(f.ifMatch) != 1 || f.ifMatch[0] != etagOf(oldFrame) {
		t.Fatalf("If-Match on the live PUT = %v, want the old ETag %s", f.ifMatch, etagOf(oldFrame))
	}
	if !bytes.Equal(f.objects[manifestsKeyDir+sha256Hex(newFrame)+manifestFrameExt], newFrame) {
		t.Fatal("the new manifest has no by-hash copy")
	}
	if f.badSigs != 0 {
		t.Fatalf("%d PUT(s) with a signature that does not match", f.badSigs)
	}
}

func TestReplaceLiveRefusesWhenTheLiveHashIsNotTheExpectedOne(t *testing.T) {
	f, srv := newFake(t)
	old := zstdFrame(t, []byte("the manifest the set expects to replace"))
	live := zstdFrame(t, []byte("someone else's manifest"))
	f.objects[livePrefix+manifestFrameName] = live
	// The recovery copy of the expected manifest is there, so only the live check can refuse.
	f.objects[manifestsKeyDir+sha256Hex(old)+manifestFrameExt] = old
	dir := replaceSet(t, zstdFrame(t, []byte("the new manifest")))
	cfg := replaceCfg(srv.URL, sha256Hex(old))
	p, err := buildPlan(dir, cfg.Prefix)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := runPublish(context.Background(), cfg, p, quietLog()); err == nil {
		t.Fatal("a live manifest whose hash is not the expected one was replaced")
	}
	if !bytes.Equal(f.objects[livePrefix+manifestFrameName], live) || len(f.ifMatch) != 0 {
		t.Fatal("the live manifest was written")
	}
}

func TestReplaceLiveRefusesARecoveryCopyThatHoldsOtherContent(t *testing.T) {
	f, srv := newFake(t)
	old := zstdFrame(t, []byte("the manifest being replaced"))
	liveFixture(t, f, old)
	f.objects[manifestsKeyDir+sha256Hex(old)+manifestFrameExt] = zstdFrame(t, []byte("not the recovery copy"))
	dir := replaceSet(t, zstdFrame(t, []byte("the new manifest")))
	cfg := replaceCfg(srv.URL, sha256Hex(old))
	p, err := buildPlan(dir, cfg.Prefix)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := runPublish(context.Background(), cfg, p, quietLog()); err == nil {
		t.Fatal("a replace whose recovery copy holds other bytes ran")
	}
	if !bytes.Equal(f.objects[livePrefix+manifestFrameName], old) || len(f.ifMatch) != 0 {
		t.Fatal("the live manifest was written against a bad recovery copy")
	}
}

func TestReplaceLiveRefusesABucketThatSendsNoETag(t *testing.T) {
	f, srv := newFake(t)
	old := zstdFrame(t, []byte("the manifest being replaced"))
	liveFixture(t, f, old)
	f.noETag = true
	dir := replaceSet(t, zstdFrame(t, []byte("the new manifest")))
	cfg := replaceCfg(srv.URL, sha256Hex(old))
	p, err := buildPlan(dir, cfg.Prefix)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := runPublish(context.Background(), cfg, p, quietLog()); err == nil {
		t.Fatal("a replace without an ETag to condition on ran")
	}
	if !bytes.Equal(f.objects[livePrefix+manifestFrameName], old) || len(f.ifMatch) != 0 {
		t.Fatal("the live manifest was written without a conditional PUT")
	}
}

func TestReplaceLiveRefusesWithoutTheRecoveryCopy(t *testing.T) {
	f, srv := newFake(t)
	old := zstdFrame(t, []byte("the manifest being replaced"))
	f.objects[livePrefix+manifestFrameName] = old // no v1/manifests copy of it
	dir := replaceSet(t, zstdFrame(t, []byte("the new manifest")))
	cfg := replaceCfg(srv.URL, sha256Hex(old))
	p, err := buildPlan(dir, cfg.Prefix)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := runPublish(context.Background(), cfg, p, quietLog()); err == nil {
		t.Fatal("a replace with no recovery copy ran")
	}
	if !bytes.Equal(f.objects[livePrefix+manifestFrameName], old) || len(f.ifMatch) != 0 {
		t.Fatal("the live manifest was written without a recovery copy")
	}
}

func TestReplaceLiveRefusesAConcurrentChangeBetweenCheckAndPut(t *testing.T) {
	f, srv := newFake(t)
	old := zstdFrame(t, []byte("the manifest being replaced"))
	liveFixture(t, f, old)
	other := zstdFrame(t, []byte("a second writer's manifest"))
	f.swapKey, f.swapTo = livePrefix+manifestFrameName, other
	dir := replaceSet(t, zstdFrame(t, []byte("the new manifest")))
	cfg := replaceCfg(srv.URL, sha256Hex(old))
	p, err := buildPlan(dir, cfg.Prefix)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := runPublish(context.Background(), cfg, p, quietLog()); err == nil {
		t.Fatal("the replace overwrote a manifest another writer had just put")
	}
	if !bytes.Equal(f.objects[livePrefix+manifestFrameName], other) {
		t.Fatal("the other writer's manifest was overwritten")
	}
}

func TestReplaceLiveReadbackMismatchFails(t *testing.T) {
	f, srv := newFake(t)
	old := zstdFrame(t, []byte("the manifest being replaced"))
	liveFixture(t, f, old)
	// Only the live PUT is corrupted: the by-hash copy is written right, so the live read-back is what must refuse.
	f.corrupt, f.corruptKey = true, livePrefix+manifestFrameName
	dir := replaceSet(t, zstdFrame(t, []byte("the new manifest")))
	cfg := replaceCfg(srv.URL, sha256Hex(old))
	p, err := buildPlan(dir, cfg.Prefix)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := runPublish(context.Background(), cfg, p, quietLog()); err == nil {
		t.Fatal("a replaced manifest that reads back with other bytes passed")
	}
}

func TestReplaceLiveRollbackIsTheSameCommandWithTheHashesSwapped(t *testing.T) {
	f, srv := newFake(t)
	oldFrame := zstdFrame(t, []byte("the manifest being replaced"))
	liveFixture(t, f, oldFrame)
	// Publish the new one first, as the forward replace did: it is now live, and its recovery copy exists.
	newFrame := zstdFrame(t, []byte("the new manifest"))
	f.objects[livePrefix+manifestFrameName] = newFrame
	f.objects[manifestsKeyDir+sha256Hex(newFrame)+manifestFrameExt] = newFrame
	// The rollback's set holds the old frame as its manifest.
	dir := replaceSet(t, oldFrame)
	cfg := replaceCfg(srv.URL, sha256Hex(newFrame))
	p, err := buildPlan(dir, cfg.Prefix)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := runPublish(context.Background(), cfg, p, quietLog()); err != nil {
		t.Fatalf("rollback: %v", err)
	}
	if !bytes.Equal(f.objects[livePrefix+manifestFrameName], oldFrame) {
		t.Fatal("the rollback did not restore the old manifest")
	}
}

func TestReplaceLiveIsRefusedUnderAnyPrefixOtherThanTheLiveOne(t *testing.T) {
	t.Parallel()
	if err := checkReplaceLive(testPrefix, sha256Hex([]byte("x"))); err == nil {
		t.Error("a staging-style prefix accepted -replace-live")
	}
	if err := checkReplaceLive(livePrefix, "not-a-hash"); err == nil {
		t.Error("a malformed expected sha256 was accepted")
	}
	if err := checkReplaceLive(livePrefix, sha256Hex([]byte("x"))); err != nil {
		t.Errorf("the live prefix with a well-formed hash: %v", err)
	}
}

// readFileT returns a file's bytes, failing the test when it cannot be read.
func readFileT(t *testing.T, name string) []byte {
	t.Helper()
	b, err := os.ReadFile(name)
	if err != nil {
		t.Fatal(err)
	}
	return b
}

// badSha is a well-formed sha256 that no test object has: the expected hash of a live manifest that is not there.
const badSha = "0000000000000000000000000000000000000000000000000000000000000000"

// logRecords returns a logger that writes JSON records into a buffer, and a function that parses what it wrote.
func logRecords(t *testing.T) (*slog.Logger, func() []map[string]any) {
	t.Helper()
	var buf bytes.Buffer
	var mu sync.Mutex
	w := writerFunc(func(p []byte) (int, error) {
		mu.Lock()
		defer mu.Unlock()
		return buf.Write(p)
	})
	log := slog.New(slog.NewJSONHandler(w, &slog.HandlerOptions{Level: slog.LevelDebug}))
	return log, func() []map[string]any {
		mu.Lock()
		defer mu.Unlock()
		var out []map[string]any
		for _, line := range strings.Split(strings.TrimSpace(buf.String()), "\n") {
			if line == "" {
				continue
			}
			var rec map[string]any
			if err := json.Unmarshal([]byte(line), &rec); err != nil {
				t.Fatalf("log line is not JSON: %q: %v", line, err)
			}
			out = append(out, rec)
		}
		return out
	}
}

type writerFunc func([]byte) (int, error)

func (f writerFunc) Write(p []byte) (int, error) { return f(p) }

// findRecord returns the first record with the message given, or nil.
func findRecord(recs []map[string]any, msg string) map[string]any {
	for _, r := range recs {
		if r["msg"] == msg {
			return r
		}
	}
	return nil
}

// TestReplaceLiveSucceedsWhileTheEdgeServesTheOldManifest is the 2026-11-15 run: the bucket takes the replace, and the
// public domain's edge keeps serving its cached copy of the old manifest. The edge also holds a cached 404 for a blob the
// bucket already has (a rerun after a partial publish) and for the new by-hash key.
//
// The code before this change failed here twice, read from main.go as it was: publishOne HEADed the public domain
// (publicStatus), got the edge's stale 404 for the present blob, PUT it with If-None-Match: *, got 412 and returned
// "key already exists; not overwritten" as an error, so runPublish stopped before any manifest. Without that blob, it
// reached replaceLive, whose read-back (publicHash after putConditional) returned the edge's old copy, and it failed with
// "readback mismatch after replace". Its live check and If-Match ETag came from the same edge copy.
func TestReplaceLiveSucceedsWhileTheEdgeServesTheOldManifest(t *testing.T) {
	f, srv := newFake(t)
	oldFrame := zstdFrame(t, []byte("the manifest being replaced"))
	liveFixture(t, f, oldFrame)
	newFrame := zstdFrame(t, []byte("the new manifest"))
	dir := replaceSet(t, newFrame)
	// One blob of the set is already in the bucket; the edge cached a 404 for it before it was written.
	present := zstdFrame(t, []byte("a blob an earlier run wrote"))
	presentKey := blobsDir + sha256Hex(present)
	f.objects[presentKey] = present
	if err := os.WriteFile(filepath.Join(dir, "blobs", "sha256", sha256Hex(present)), present, testFilePerm); err != nil {
		t.Fatal(err)
	}
	byHashKey := manifestsKeyDir + sha256Hex(newFrame) + manifestFrameExt
	liveKey := livePrefix + manifestFrameName
	f.edge = map[string]edgeCopy{
		liveKey:    {status: http.StatusOK, body: oldFrame},
		presentKey: {status: http.StatusNotFound},
		byHashKey:  {status: http.StatusNotFound},
	}
	cfg := replaceCfg(srv.URL, sha256Hex(oldFrame))
	p, err := buildPlan(dir, cfg.Prefix)
	if err != nil {
		t.Fatal(err)
	}
	log, records := logRecords(t)
	if _, err := runPublish(context.Background(), cfg, p, log); err != nil {
		t.Fatalf("runPublish: %v", err)
	}
	if !bytes.Equal(f.objects[liveKey], newFrame) {
		t.Fatal("the bucket's live manifest was not replaced")
	}
	if len(f.ifMatch) != 1 || f.ifMatch[0] != etagOf(oldFrame) {
		t.Fatalf("If-Match = %v, want the bucket's ETag of the old manifest %s", f.ifMatch, etagOf(oldFrame))
	}
	for _, k := range f.putOrder {
		if k == presentKey {
			t.Fatalf("the present blob %s was put again", k)
		}
	}
	if f.publicReqs != 1 || f.badSigs != 0 {
		t.Fatalf("public domain requests = %d, bad signatures = %d; want 1 (the report) and 0", f.publicReqs, f.badSigs)
	}
	rec := findRecord(records(), "public domain still serves another copy")
	if rec == nil || rec["level"] != "INFO" || rec["sha256"] != sha256Hex(oldFrame) || rec["want_sha256"] != sha256Hex(newFrame) ||
		rec["cf_cache_status"] != "HIT" || rec["age"] != testEdgeAge || !strings.Contains(fmt.Sprint(rec["consequence"]), "purged") {
		t.Fatalf("public report record = %v", rec)
	}
}

// TestReplaceLiveFailsWhenTheBucketStillHoldsTheOldManifest: the PUT answers 200 but the bucket keeps the old bytes, while
// the edge already serves the new ones. The read-back is the bucket's, so the run fails, and the public domain is not read.
func TestReplaceLiveFailsWhenTheBucketStillHoldsTheOldManifest(t *testing.T) {
	f, srv := newFake(t)
	oldFrame := zstdFrame(t, []byte("the manifest being replaced"))
	liveFixture(t, f, oldFrame)
	newFrame := zstdFrame(t, []byte("the new manifest"))
	liveKey := livePrefix + manifestFrameName
	f.dropKey = liveKey
	f.edge = map[string]edgeCopy{liveKey: {status: http.StatusOK, body: newFrame}}
	dir := replaceSet(t, newFrame)
	cfg := replaceCfg(srv.URL, sha256Hex(oldFrame))
	p, err := buildPlan(dir, cfg.Prefix)
	if err != nil {
		t.Fatal(err)
	}
	_, err = runPublish(context.Background(), cfg, p, quietLog())
	if err == nil || !strings.Contains(err.Error(), "readback mismatch after replace") || !strings.Contains(err.Error(), sha256Hex(oldFrame)) {
		t.Fatalf("err = %v; want a readback mismatch naming the old sha256", err)
	}
	if f.publicReqs != 0 {
		t.Fatalf("public domain requests = %d, want 0: no check reads it", f.publicReqs)
	}
}

// TestAStaleEdge404DoesNotMakeAPresentBlobAnError: the bucket holds the blob and the edge has a cached 404 for it. The
// presence check is the bucket's, so the blob is skipped with no PUT and the public domain is never asked.
func TestAStaleEdge404DoesNotMakeAPresentBlobAnError(t *testing.T) {
	f, srv := newFake(t)
	dir, _ := writeSet(t, [][]byte{[]byte("already there")})
	frame := zstdFrame(t, []byte("already there"))
	key := blobKeyOf(frame)
	f.objects[key] = frame
	f.edge = map[string]edgeCopy{key: {status: http.StatusNotFound}}
	cfg := testConfig(srv.URL)
	p, err := buildPlan(dir, cfg.Prefix)
	if err != nil {
		t.Fatal(err)
	}
	sum, err := runPublish(context.Background(), cfg, p, quietLog())
	if err != nil {
		t.Fatalf("runPublish: %v", err)
	}
	if sum.Skipped != 1 || f.publicReqs != 0 || f.badSigs != 0 {
		t.Fatalf("skipped = %d, public requests = %d, bad signatures = %d; want 1, 0, 0", sum.Skipped, f.publicReqs, f.badSigs)
	}
	for _, k := range f.putOrder {
		if k == key {
			t.Fatal("the present blob was put")
		}
	}
}

// TestAPutRefusedBecauseTheKeyExistsIsASkipWhenTheBucketHoldsTheSameBytes: HEAD says absent, the PUT gets 412, and a
// signed GET shows the bucket holds exactly these bytes, so the object counts as present. (Other bytes under the key are
// an error: TestNeverOverwritesAnObjectThatAppearsBeforeThePut.)
func TestAPutRefusedBecauseTheKeyExistsIsASkipWhenTheBucketHoldsTheSameBytes(t *testing.T) {
	f, srv := newFake(t)
	dir, manifest := writeSet(t, [][]byte{[]byte("the blob")})
	frame := zstdFrame(t, []byte("the blob"))
	f.objects[blobKeyOf(frame)] = frame
	byHash := manifestsKeyDir + sha256Hex(manifest) + manifestFrameExt
	f.objects[byHash] = manifest
	f.race = true
	cfg := testConfig(srv.URL)
	p, err := buildPlan(dir, cfg.Prefix)
	if err != nil {
		t.Fatal(err)
	}
	const wantSkipped, wantUploaded = 2, 1 // the blob and the by-hash manifest; the current manifest is new
	sum, err := runPublish(context.Background(), cfg, p, quietLog())
	if err != nil {
		t.Fatalf("runPublish: %v", err)
	}
	if sum.Skipped != wantSkipped || sum.Uploaded != wantUploaded {
		t.Fatalf("summary = %+v, want skipped %d uploaded %d", sum, wantSkipped, wantUploaded)
	}
	if !bytes.Equal(f.objects[blobKeyOf(frame)], frame) {
		t.Fatal("the stored blob changed")
	}
}

// TestPutsCarrySignedCacheControl: every PUT carries the Cache-Control for its kind of key, and cache-control is one of the
// headers its signature covers.
func TestPutsCarrySignedCacheControl(t *testing.T) {
	f, srv := newFake(t)
	dir, manifest := writeSet(t, [][]byte{[]byte("alpha")})
	cfg := testConfig(srv.URL)
	cfg.Prefix = livePrefix
	p, err := buildPlan(dir, cfg.Prefix)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := runPublish(context.Background(), cfg, p, quietLog()); err != nil {
		t.Fatalf("runPublish: %v", err)
	}
	want := map[string]string{
		blobsDir + sha256Hex(zstdFrame(t, []byte("alpha"))):      cacheControlImmutable,
		manifestsKeyDir + sha256Hex(manifest) + manifestFrameExt: cacheControlImmutable,
		livePrefix + manifestFrameName:                           cacheControlMutable,
	}
	if f.badSigs != 0 || len(f.putHeaders) != len(want) {
		t.Fatalf("bad signatures = %d, PUTs = %v; want 0 and %d keys", f.badSigs, f.putOrder, len(want))
	}
	for key, cc := range want {
		h := f.putHeaders[key]
		if h == nil {
			t.Fatalf("no PUT of %s", key)
		}
		if got := h.Get(cacheControlName); got != cc {
			t.Errorf("%s: Cache-Control = %q, want %q", key, got, cc)
		}
		signed := signedHeadersOf(h.Get("Authorization"))
		found := false
		for _, n := range signed {
			found = found || n == strings.ToLower(cacheControlName)
		}
		if !found {
			t.Errorf("%s: SignedHeaders %v does not cover cache-control", key, signed)
		}
	}
}

// TestTheSignatureCoversTheCacheControlValue: a signed PUT whose Cache-Control is changed after signing no longer matches
// its signature, so the bucket would refuse it.
func TestTheSignatureCoversTheCacheControlValue(t *testing.T) {
	cfg := testConfig("http://bucket.invalid")
	req, err := bucketRequest(context.Background(), cfg, http.MethodPut, livePrefix+manifestFrameName, nil)
	if err != nil {
		t.Fatal(err)
	}
	req.Header.Set(cacheControlName, cacheControlMutable)
	signV4(req, cfg, time.Now())
	req.Host = req.URL.Host
	if !signatureMatches(req) {
		t.Fatal("the signed request does not match its own signature")
	}
	req.Header.Set(cacheControlName, cacheControlImmutable)
	if signatureMatches(req) {
		t.Fatal("a changed Cache-Control still matches the signature")
	}
}
