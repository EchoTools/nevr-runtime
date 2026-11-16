// Command cargo-publish puts an extracted object set and its content manifest into an S3 bucket under one key prefix
// (P2 of the cdn-all-assets test; the cargo bucket is served at a public domain):
//
//	NEVR_CARGO_S3_ENDPOINT=... NEVR_CARGO_S3_BUCKET=... NEVR_CARGO_S3_ACCESS_KEY_ID=... \
//	NEVR_CARGO_S3_SECRET_ACCESS_KEY=... NEVR_CARGO_PUBLIC_BASE=https://cargo.echo.taxi \
//	NEVR_CARGO_PREFIX=v1/allassets/2026-11-12/ \
//	cargo-publish -dir <extract>/quest [-workers 8]
//
//	NEVR_CARGO_PREFIX=v1/allassets/2026-11-12/ cargo-publish -dir <extract>/quest -dry-run
//
// The set is <dir>/blobs/sha256/<64 hex>: each file must start with the zstd frame magic and hash to its name. The
// manifest goes up only as the zstd frame <dir>/manifest.json.zst, never as plain JSON, at two keys: <prefix>manifest.json.zst
// and v1/manifests/<sha256 of the frame>.json.zst (under the staging prefix v1-staging/ the second key is
// <prefix>manifests/<sha256 of the frame>.json.zst, so nothing outside the prefix is written). A plain manifest.json in the set is not read.
//
// Blobs are written at <prefix>blobs/sha256/<hex>, except under the prefix v1/ (the layout of a client with the default
// base URL, docs/adr/0045-cdn-texture-delivery.md decision 5): there the blobs are at blobs/sha256/<hex> in the bucket root and
// the manifests at v1/manifests/<sha> and v1/manifest.json.zst. The prefix v2/ (the manifest of labels that carry scripts)
// shares those same root blobs and writes v2/manifests/<sha>.json.zst and v2/manifest.json.zst; v1 keys are never written under it. Every blob is sent first, then the by-hash manifest, then
// <prefix>manifest.json.zst, and an error stops the later phases.
//
// Every read whose answer decides a write or proves one goes to the bucket through the S3 API on the endpoint, signed like
// the PUT: never through the public domain, whose edge cache can serve a copy (or a 404) older than the bucket. Per object:
// a signed HEAD of the bucket key (200 skips the object, 404 puts it, any other status is an error), a PUT with
// If-None-Match: * (the bucket refuses to overwrite), then a signed GET of the bucket key whose SHA-256 and size must match.
// A PUT refused with 412 (the key exists) is a skip when a signed GET shows the stored bytes hash to the object's sha256,
// and an error otherwise. Nothing is deleted, nothing is overwritten (a manifest key that is present with other content is an
// error, so this tool never replaces a manifest that exists), nothing outside the prefix is written except, under v1/, the blobs
// directory of the bucket root and v1/manifests/ (and, under any other prefix, v1/manifests/), and no secret is logged. Dry run checks and hashes the set and prints the counts and bytes it would send,
// without the network and without any credential.
//
// Each PUT carries a Cache-Control the bucket stores and the public domain sends with the object (signed with the request):
// cacheControlMutable for <prefix>manifest.json.zst, the one key whose content changes, and cacheControlImmutable for a blob
// or a by-hash manifest, whose key is the hash of its bytes.
//
// -replace-live <sha256> is the one exception to "never overwrites": with NEVR_CARGO_PREFIX v1/ it puts the set's manifest
// over v1/manifest.json.zst, and only while the bucket's object holds <sha256> and its recovery copy
// v1/manifests/<sha256>.json.zst holds the same bytes, both read by signed GET. The PUT is conditional on the ETag the bucket
// sent with that GET (If-Match), so a write that landed after the check is refused (412) rather than overwritten; the bucket's
// object is read back and must hash to the new manifest. A bucket that sends no ETag is refused before the PUT. Then the public
// URL is read once, as a report and not a check: an Info record gives what it serves (sha256, cf-cache-status, age) and says
// when clients get the new manifest. Rollback is the same command with the hashes swapped (the old manifest as the set's, the
// new one as <sha256>). No object is deleted. The bucket must honour If-Match on PUT.
//
// Records are JSON on standard error (Info per uploaded object, for the public report and for the summary, Debug per skipped
// object, Error for each failure); the run summary is one JSON line on standard output. Exit 1 names each failure.
package main

import (
	"bytes"
	"context"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"sync"
	"time"
)

const (
	defaultWorkers    = 8
	defaultRegion     = "auto"
	requestTimeout    = 5 * time.Minute
	maxErrorBodyBytes = 4096
	sha256HexLen      = 2 * sha256.Size // 64 hex characters
	amzDateLayout     = "20060102T150405Z"
	amzDateStampLen   = 8 // the YYYYMMDD prefix of amzDateLayout
	unsignedPayload   = "UNSIGNED-PAYLOAD"
	blobsDir          = "blobs/sha256/"
	plainManifestName = "manifest.json"
	manifestFrameName = "manifest.json.zst"
	manifestsKeyDir   = "v1/manifests/"
	manifestsKeyDirV2 = "v2/manifests/"
	livePrefix        = "v1/"         // the ADR 0045 layout: blobs at the bucket root, manifests under v1/
	livePrefixV2      = "v2/"         // the same layout for the schema whose labels carry scripts: shared blobs, manifests under v2/
	stagingPrefix     = "v1-staging/" // a staging set stays whole under it: its by-hash manifest is kept at <prefix>manifests/
	manifestFrameExt  = ".json.zst"
	liveManifestKey   = "v1/manifest.json" // never written by this tool
	logLevelDefault   = slog.LevelInfo
)

// zstdMagic is the first four bytes of every zstd frame (RFC 8878 section 3.1.1).
var zstdMagic = [4]byte{0x28, 0xb5, 0x2f, 0xfd}

var hexName = regexp.MustCompile(fmt.Sprintf("^[0-9a-f]{%d}$", sha256HexLen))

type config struct {
	Endpoint    string
	Bucket      string
	Region      string
	AccessKeyID string
	SecretKey   string
	PublicBase  string
	Prefix      string
	Workers     int
	HTTP        *http.Client
	// ReplaceLive, when set, is the sha256 the live manifest must hold for it to be replaced (-replace-live).
	ReplaceLive string
}

// loadConfig reads every setting from the environment through getenv. A missing one is refused by name.
func loadConfig(getenv func(string) string) (config, error) {
	var missing []string
	need := func(name string) string {
		v := strings.TrimSpace(getenv(name))
		if v == "" {
			missing = append(missing, name)
		}
		return v
	}
	c := config{
		Endpoint:    need("NEVR_CARGO_S3_ENDPOINT"),
		Bucket:      need("NEVR_CARGO_S3_BUCKET"),
		AccessKeyID: need("NEVR_CARGO_S3_ACCESS_KEY_ID"),
		SecretKey:   need("NEVR_CARGO_S3_SECRET_ACCESS_KEY"),
		PublicBase:  need("NEVR_CARGO_PUBLIC_BASE"),
		Prefix:      need("NEVR_CARGO_PREFIX"),
	}
	if len(missing) > 0 {
		return config{}, fmt.Errorf("missing environment variable(s): %s", strings.Join(missing, ", "))
	}
	c.Region = strings.TrimSpace(getenv("NEVR_CARGO_S3_REGION"))
	if c.Region == "" {
		c.Region = defaultRegion
	}
	if err := checkPrefix(c.Prefix); err != nil {
		return config{}, err
	}
	c.Workers = defaultWorkers
	c.HTTP = &http.Client{Timeout: requestTimeout}
	return c, nil
}

// checkReplaceLive refuses -replace-live unless the prefix is the live one (its only key is v1/manifest.json.zst) and the
// expected hash is a sha256 in lower-case hex.
func checkReplaceLive(prefix, expected string) error {
	if prefix != livePrefix {
		return fmt.Errorf("-replace-live replaces %s%s and nothing else: NEVR_CARGO_PREFIX must be %s, not %q", livePrefix, manifestFrameName, livePrefix, prefix)
	}
	if !hexName.MatchString(expected) {
		return fmt.Errorf("-replace-live needs the live manifest's sha256 as %d lower-case hex characters", sha256HexLen)
	}
	return nil
}

// checkPrefix refuses a prefix that is not a relative folder path ending in "/", so no key can leave it.
func checkPrefix(p string) error {
	if !strings.HasSuffix(p, "/") || strings.HasPrefix(p, "/") || strings.Contains(p, "..") {
		return fmt.Errorf("NEVR_CARGO_PREFIX %q must be a relative path ending in /", p)
	}
	return nil
}

type object struct {
	Key   string
	Local string
	SHA   string
	Size  int64
}

type plan struct {
	Objects []object
	Blobs   int
	Bytes   int64
}

// checkZstd refuses a file whose first four bytes are not the zstd frame magic.
func checkZstd(path string) error {
	f, err := os.Open(path)
	if err != nil {
		return err
	}
	defer f.Close()
	var head [len(zstdMagic)]byte
	if _, err := io.ReadFull(f, head[:]); err != nil {
		return fmt.Errorf("shorter than the four-byte zstd magic: %w", err)
	}
	if !bytes.Equal(head[:], zstdMagic[:]) {
		return fmt.Errorf("first four bytes are %x, not the zstd frame magic 28b52ffd", head[:])
	}
	return nil
}

// buildPlan checks and hashes the set. A blob that is not a zstd frame, whose bytes do not hash to its name, or whose
// name is not 64 lowercase hex is refused before anything is sent. The manifest is the zstd frame only.
func buildPlan(dir, prefix string) (plan, error) {
	if err := checkPrefix(prefix); err != nil {
		return plan{}, err
	}
	bdir := filepath.Join(dir, "blobs", "sha256")
	entries, err := os.ReadDir(bdir)
	if err != nil {
		return plan{}, fmt.Errorf("read blob directory: %w", err)
	}
	var p plan
	for _, e := range entries {
		name := e.Name()
		if !e.Type().IsRegular() {
			return plan{}, fmt.Errorf("blob entry %s is not a regular file", name)
		}
		if !hexName.MatchString(name) {
			return plan{}, fmt.Errorf("blob name %q is not %d lowercase hex characters", name, sha256HexLen)
		}
		local := filepath.Join(bdir, name)
		if err := checkZstd(local); err != nil {
			return plan{}, fmt.Errorf("blob %s refused, not a zstd frame: %w", name, err)
		}
		sum, size, err := hashFile(local)
		if err != nil {
			return plan{}, fmt.Errorf("hash %s: %w", name, err)
		}
		if sum != name {
			return plan{}, fmt.Errorf("blob %s holds bytes that hash to %s", name, sum)
		}
		p.Objects = append(p.Objects, object{Key: blobBase(prefix) + blobsDir + name, Local: local, SHA: sum, Size: size})
		p.Blobs++
		p.Bytes += size
	}
	frame := filepath.Join(dir, manifestFrameName)
	if _, err := os.Stat(frame); err != nil {
		if os.IsNotExist(err) {
			return plan{}, fmt.Errorf("no %s in %s: a plain %s is never uploaded, the manifest goes up only as the zstd frame",
				manifestFrameName, dir, plainManifestName)
		}
		return plan{}, fmt.Errorf("manifest frame: %w", err)
	}
	if err := checkZstd(frame); err != nil {
		return plan{}, fmt.Errorf("manifest frame refused, not a zstd frame: %w", err)
	}
	sum, size, err := hashFile(frame)
	if err != nil {
		return plan{}, fmt.Errorf("hash manifest frame: %w", err)
	}
	keys := []string{prefix + manifestFrameName, manifestsDirFor(prefix) + sum + manifestFrameExt}
	for _, k := range keys {
		if k == liveManifestKey {
			return plan{}, fmt.Errorf("manifest key %s is the live manifest; refused", k)
		}
		p.Objects = append(p.Objects, object{Key: k, Local: frame, SHA: sum, Size: size})
		p.Bytes += size
	}
	sort.Slice(p.Objects, func(i, j int) bool { return p.Objects[i].Key < p.Objects[j].Key })
	return p, nil
}

// blobBase is where a set's blobs go: under its prefix, except for the live prefixes v1/ and v2/, whose blobs are the one
// shared, immutable set at the bucket root
// (docs/adr/0045-cdn-texture-delivery.md decision 5: a client with the default base URL reads <base>blobs/sha256/<hex>).
func blobBase(prefix string) string {
	if prefix == livePrefix || prefix == livePrefixV2 {
		return ""
	}
	return prefix
}

// manifestsDirFor is the folder the by-hash copy of the manifest goes to: v2/manifests/ for the prefix v2/, else the shared v1/manifests/, except for a set under
// the staging prefix, which writes nothing outside it (what reaches v1/ is a separate, deliberate publish).
func manifestsDirFor(prefix string) string {
	if prefix == livePrefixV2 {
		return manifestsKeyDirV2
	}
	if strings.HasPrefix(prefix, stagingPrefix) {
		return prefix + "manifests/"
	}
	return manifestsKeyDir
}

func hashFile(p string) (string, int64, error) {
	f, err := os.Open(p)
	if err != nil {
		return "", 0, err
	}
	defer f.Close()
	h := sha256.New()
	n, err := io.Copy(h, f)
	if err != nil {
		return "", 0, err
	}
	return hex.EncodeToString(h.Sum(nil)), n, nil
}

type summary struct {
	Dry           bool   `json:"dry_run"`
	Objects       int    `json:"objects"`
	Bytes         int64  `json:"bytes"`
	Blobs         int    `json:"blobs"`
	Uploaded      int    `json:"uploaded"`
	UploadedBytes int64  `json:"uploaded_bytes"`
	Skipped       int    `json:"skipped_present"`
	SkippedBytes  int64  `json:"skipped_bytes"`
	Verified      int    `json:"verified"`
	Prefix        string `json:"prefix"`
}

func dryRunSummary(p plan) summary {
	return summary{Dry: true, Objects: len(p.Objects), Bytes: p.Bytes, Blobs: p.Blobs}
}

// runPublish sends every object of the plan that the bucket does not already hold (a signed HEAD on the bucket key decides,
// never the public domain). It keeps going past a failed object so every failure is named, then returns them joined.
func runPublish(ctx context.Context, cfg config, p plan, log *slog.Logger) (summary, error) {
	sum := summary{Objects: len(p.Objects), Bytes: p.Bytes, Blobs: p.Blobs, Prefix: cfg.Prefix}
	workers := cfg.Workers
	if workers < 1 {
		workers = 1
	}
	var mu sync.Mutex
	var errs []error
	// send runs the objects over the workers and returns when every one is done.
	send := func(objs []object) {
		jobs := make(chan object)
		var wg sync.WaitGroup
		for i := 0; i < workers; i++ {
			wg.Add(1)
			go func() {
				defer wg.Done()
				for o := range jobs {
					res, err := publishOne(ctx, cfg, o, log)
					mu.Lock()
					switch {
					case err != nil:
						errs = append(errs, err)
					case res == "skipped":
						sum.Skipped++
						sum.SkippedBytes += o.Size
					default:
						sum.Uploaded++
						sum.UploadedBytes += o.Size
						sum.Verified++
					}
					mu.Unlock()
				}
			}()
		}
		for _, o := range objs {
			if ctx.Err() != nil {
				break
			}
			jobs <- o
		}
		close(jobs)
		wg.Wait()
	}
	// Every blob is in place before any manifest names it; the by-hash copy goes before the current one.
	var blobs, byHash, current []object
	for _, o := range p.Objects {
		switch {
		case strings.HasSuffix(o.Key, manifestFrameName):
			current = append(current, o)
		case strings.HasSuffix(o.Key, manifestFrameExt):
			byHash = append(byHash, o)
		default:
			blobs = append(blobs, o)
		}
	}
	send(blobs)
	if len(errs) == 0 {
		send(byHash)
	}
	if len(errs) == 0 {
		send(current)
	}
	log.Info("publish finished", "objects", sum.Objects, "uploaded", sum.Uploaded, "skipped_present", sum.Skipped,
		"verified", sum.Verified, "failures", len(errs), "prefix", cfg.Prefix)
	if err := ctx.Err(); err != nil {
		errs = append(errs, err)
	}
	return sum, errors.Join(errs...)
}

// publishOne returns "skipped" when the object is present, "uploaded" after a put that read back by hash. Every read here
// is a signed S3 call on the bucket key (bucketHead, bucketGet), never the public domain.
func publishOne(ctx context.Context, cfg config, o object, log *slog.Logger) (string, error) {
	if cfg.ReplaceLive != "" && o.Key == livePrefix+manifestFrameName {
		return replaceLive(ctx, cfg, o, log)
	}
	present, err := bucketHead(ctx, cfg, o.Key)
	if err != nil {
		log.Error("head failed", "key", o.Key, "error", err.Error())
		return "", fmt.Errorf("%s: head: %w", o.Key, err)
	}
	if present {
		// A blob's key is its hash, so presence is identity. A manifest key is a name: what is there may be another set's.
		if strings.HasSuffix(o.Key, manifestFrameName) {
			if err := checkStored(ctx, cfg, o, log); err != nil {
				return "", err
			}
		}
		log.Debug("object present, skipped", "key", o.Key, "bytes", o.Size)
		return "skipped", nil
	}
	if err := putObject(ctx, cfg, o); err != nil {
		if errors.Is(err, errPreconditionFailed) {
			// The key appeared between the HEAD and the PUT (another writer). It is the same object only when its stored
			// bytes hash to ours.
			if err := checkStored(ctx, cfg, o, log); err != nil {
				return "", err
			}
			log.Info("put refused because the key exists, and the bucket holds these bytes: skipped", "key", o.Key,
				"bytes", o.Size, "sha256", o.SHA)
			return "skipped", nil
		}
		log.Error("put failed", "key", o.Key, "error", err.Error())
		return "", fmt.Errorf("%s: put: %w", o.Key, err)
	}
	got, err := bucketGet(ctx, cfg, o.Key)
	if err != nil {
		log.Error("readback failed", "key", o.Key, "error", err.Error())
		return "", fmt.Errorf("%s: readback: %w", o.Key, err)
	}
	if got.SHA != o.SHA || got.Size != o.Size {
		log.Error("readback mismatch", "key", o.Key, "want_sha256", o.SHA, "got_sha256", got.SHA, "want_bytes", o.Size, "got_bytes", got.Size)
		return "", fmt.Errorf("%s: readback mismatch: want sha256 %s and %d bytes, got %s and %d bytes", o.Key, o.SHA, o.Size, got.SHA, got.Size)
	}
	log.Info("object uploaded", "key", o.Key, "bytes", o.Size, "sha256", o.SHA)
	return "uploaded", nil
}

// checkStored reads the bucket key by signed GET and returns nil only when it holds the object's bytes (sha256 and size).
func checkStored(ctx context.Context, cfg config, o object, log *slog.Logger) error {
	got, err := bucketGet(ctx, cfg, o.Key)
	if err != nil {
		log.Error("present, read back failed", "key", o.Key, "error", err.Error())
		return fmt.Errorf("%s: present, read back: %w", o.Key, err)
	}
	if got.SHA != o.SHA || got.Size != o.Size {
		log.Error("present with other content", "key", o.Key, "want_sha256", o.SHA, "got_sha256", got.SHA,
			"want_bytes", o.Size, "got_bytes", got.Size)
		return fmt.Errorf("%s: present with other content (sha256 %s, %d bytes), not overwritten; the set is not served under this key", o.Key, got.SHA, got.Size)
	}
	return nil
}

// replaceLive puts the set's manifest over the live one, once three checks pass, each on the bucket by signed GET: the live
// object holds the expected sha256, its recovery copy <prefix>manifests/<expected>.json.zst holds it too, and the PUT is
// conditional on the ETag the bucket sent with the live object, so a write that landed after the check is refused (412), not
// overwritten. The bucket's read-back must hash to the new manifest. The public URL is then read once as a report
// (reportPublic), never as a check. Rollback is the same call with the hashes swapped: the old manifest is the set's, the
// new one is expected.
func replaceLive(ctx context.Context, cfg config, o object, log *slog.Logger) (string, error) {
	live, err := bucketGet(ctx, cfg, o.Key)
	if err != nil {
		return "", fmt.Errorf("%s: read the live manifest: %w", o.Key, err)
	}
	if live.SHA != cfg.ReplaceLive {
		log.Error("live manifest is not the expected one", "key", o.Key, "want_sha256", cfg.ReplaceLive, "got_sha256", live.SHA)
		return "", fmt.Errorf("%s: live manifest has sha256 %s, not the expected %s; nothing written", o.Key, live.SHA, cfg.ReplaceLive)
	}
	recovery := manifestsKeyDir + cfg.ReplaceLive + manifestFrameExt
	rec, err := bucketGet(ctx, cfg, recovery)
	if err != nil {
		return "", fmt.Errorf("%s: the recovery copy: %w", recovery, err)
	}
	if rec.SHA != cfg.ReplaceLive {
		return "", fmt.Errorf("%s: recovery copy has sha256 %s, not %s; nothing written", recovery, rec.SHA, cfg.ReplaceLive)
	}
	if live.ETag == "" {
		return "", fmt.Errorf("%s: the bucket sent no ETag, so the PUT cannot be conditional; nothing written", o.Key)
	}
	if err := putConditional(ctx, cfg, o, ifMatchHeader, live.ETag); err != nil {
		log.Error("replace refused", "key", o.Key, "error", err.Error())
		return "", fmt.Errorf("%s: replace: %w", o.Key, err)
	}
	got, err := bucketGet(ctx, cfg, o.Key)
	if err != nil {
		return "", fmt.Errorf("%s: readback: %w", o.Key, err)
	}
	if got.SHA != o.SHA || got.Size != o.Size {
		return "", fmt.Errorf("%s: readback mismatch after replace: want sha256 %s and %d bytes, got %s and %d bytes", o.Key, o.SHA, o.Size, got.SHA, got.Size)
	}
	log.Info("live manifest replaced", "key", o.Key, "old_sha256", live.SHA, "new_sha256", o.SHA, "bytes", o.Size)
	reportPublic(ctx, cfg, o, log)
	return "uploaded", nil
}

// reportPublic reads the key once from the public domain and logs what it serves. It is a report, not a check: the bucket
// already holds the new bytes (replaceLive read them back), and an edge cache in front of the public domain may keep serving
// its older copy until that copy expires.
func reportPublic(ctx context.Context, cfg config, o object, log *slog.Logger) {
	got, err := publicGet(ctx, cfg, o.Key)
	if err != nil {
		log.Warn("public domain not read after the replace", "key", o.Key, "url", publicURL(cfg, o.Key), "error", err.Error(),
			"consequence", "what clients are served is not known; the bucket holds the new manifest")
		return
	}
	if got.SHA == o.SHA && got.Size == o.Size {
		log.Info("public domain serves the new manifest", "key", o.Key, "url", publicURL(cfg, o.Key), "sha256", got.SHA,
			"cf_cache_status", got.CacheStatus, "age", got.Age,
			"consequence", "clients that fetch it now get the new manifest")
		return
	}
	log.Info("public domain still serves another copy", "key", o.Key, "url", publicURL(cfg, o.Key), "sha256", got.SHA,
		"bytes", got.Size, "want_sha256", o.SHA, "cf_cache_status", got.CacheStatus, "age", got.Age,
		"consequence", "the bucket holds the new manifest; clients get it when the edge copy expires, or at once if the zone's cache for this key is purged")
}

func publicURL(cfg config, key string) string {
	return strings.TrimRight(cfg.PublicBase, "/") + "/" + key
}

// objectInfo is what a GET tells about an object: its SHA-256, its size and its ETag, and, from the public domain only,
// the edge cache's cf-cache-status and Age headers.
type objectInfo struct {
	SHA         string
	Size        int64
	ETag        string
	CacheStatus string
	Age         string
}

const (
	cacheStatusHeader = "Cf-Cache-Status" // Cloudflare's answer to "did the edge serve its own copy"
	ageHeader         = "Age"
	cacheControlName  = "Cache-Control"
	// cacheControlMutable is sent with <prefix>manifest.json.zst, the one key whose content changes (-replace-live). It bounds
	// how long an edge or a client may reuse an older copy after a replace to one minute (if no zone rule overrides the
	// origin's header), and still lets the edge absorb a burst of clients starting at once. The manifest is small and read once per start, so a short life costs little.
	cacheControlMutable = "public, max-age=60"
	// cacheControlImmutable is sent with a blob or a by-hash manifest: the key is the sha256 of the bytes, so the bytes under
	// it never change. A year is the customary max-age for content-addressed files, and immutable (RFC 8246) tells a cache not
	// to revalidate them while they are fresh.
	cacheControlImmutable = "public, max-age=31536000, immutable"
)

// cacheControlFor is the Cache-Control a PUT of the key carries: short for the named current manifest, long and immutable for
// a content-addressed key.
func cacheControlFor(key string) string {
	if strings.HasSuffix(key, manifestFrameName) {
		return cacheControlMutable
	}
	return cacheControlImmutable
}

// errNotFound is a 404 answer to a GET.
var errNotFound = errors.New("HTTP 404")

// bucketRequest makes a signed S3 request for the bucket key (path style: <endpoint>/<bucket>/<key>).
func bucketRequest(ctx context.Context, cfg config, method, key string, body io.Reader) (*http.Request, error) {
	u, err := url.Parse(strings.TrimRight(cfg.Endpoint, "/"))
	if err != nil {
		return nil, fmt.Errorf("endpoint: %w", err)
	}
	u.Path = "/" + cfg.Bucket + "/" + key
	return http.NewRequestWithContext(ctx, method, u.String(), body)
}

// bucketHead reports whether the bucket holds the key, by a signed S3 HEAD: true on 200, false on 404, an error otherwise.
func bucketHead(ctx context.Context, cfg config, key string) (bool, error) {
	req, err := bucketRequest(ctx, cfg, http.MethodHead, key, nil)
	if err != nil {
		return false, err
	}
	signV4(req, cfg, time.Now())
	resp, err := cfg.HTTP.Do(req)
	if err != nil {
		return false, err
	}
	resp.Body.Close()
	switch resp.StatusCode {
	case http.StatusOK:
		return true, nil
	case http.StatusNotFound:
		return false, nil
	default:
		return false, fmt.Errorf("HEAD answered HTTP %d, not 200 or 404", resp.StatusCode)
	}
}

// bucketGet fetches the key from the bucket by a signed S3 GET and returns its SHA-256, byte count and ETag. A 404 is
// errNotFound.
func bucketGet(ctx context.Context, cfg config, key string) (objectInfo, error) {
	req, err := bucketRequest(ctx, cfg, http.MethodGet, key, nil)
	if err != nil {
		return objectInfo{}, err
	}
	signV4(req, cfg, time.Now())
	return getInfo(cfg, req)
}

// publicGet fetches the key from the public domain. Only reportPublic calls it: its answer may be an edge cache's copy.
func publicGet(ctx context.Context, cfg config, key string) (objectInfo, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, publicURL(cfg, key), nil)
	if err != nil {
		return objectInfo{}, err
	}
	return getInfo(cfg, req)
}

// getInfo sends the GET and hashes the body.
func getInfo(cfg config, req *http.Request) (objectInfo, error) {
	resp, err := cfg.HTTP.Do(req)
	if err != nil {
		return objectInfo{}, err
	}
	defer resp.Body.Close()
	switch resp.StatusCode {
	case http.StatusOK:
	case http.StatusNotFound:
		return objectInfo{}, errNotFound
	default:
		return objectInfo{}, fmt.Errorf("HTTP %d", resp.StatusCode)
	}
	h := sha256.New()
	n, err := io.Copy(h, resp.Body)
	if err != nil {
		return objectInfo{}, err
	}
	return objectInfo{
		SHA: hex.EncodeToString(h.Sum(nil)), Size: n, ETag: resp.Header.Get("ETag"),
		CacheStatus: resp.Header.Get(cacheStatusHeader), Age: resp.Header.Get(ageHeader),
	}, nil
}

const (
	ifNoneMatchHeader = "If-None-Match"
	ifMatchHeader     = "If-Match"
	ifNoneMatchAny    = "*"
)

// errPreconditionFailed is a PUT the bucket refused with 412.
var errPreconditionFailed = errors.New("HTTP 412")

// putObject writes the object with a conditional PUT: the bucket refuses (412) when the key already exists.
func putObject(ctx context.Context, cfg config, o object) error {
	return putConditional(ctx, cfg, o, ifNoneMatchHeader, ifNoneMatchAny)
}

// putConditional writes the object with the precondition header given: If-None-Match "*" refuses an existing key, and
// If-Match an ETag refuses a key whose content is not that one (412 in both cases, errPreconditionFailed).
func putConditional(ctx context.Context, cfg config, o object, precondition, value string) error {
	f, err := os.Open(o.Local)
	if err != nil {
		return err
	}
	defer f.Close()
	req, err := bucketRequest(ctx, cfg, http.MethodPut, o.Key, f)
	if err != nil {
		return err
	}
	req.ContentLength = o.Size
	req.Header.Set(precondition, value)
	req.Header.Set("Content-Type", "application/octet-stream")
	req.Header.Set(cacheControlName, cacheControlFor(o.Key))
	signV4(req, cfg, time.Now())
	resp, err := cfg.HTTP.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(io.LimitReader(resp.Body, maxErrorBodyBytes))
	switch resp.StatusCode {
	case http.StatusOK, http.StatusCreated:
		return nil
	case http.StatusPreconditionFailed:
		if precondition == ifMatchHeader {
			return fmt.Errorf("the object changed since it was read; not overwritten (%w)", errPreconditionFailed)
		}
		return fmt.Errorf("key already exists; not overwritten (%w)", errPreconditionFailed)
	default:
		return fmt.Errorf("HTTP %d: %s", resp.StatusCode, strings.TrimSpace(string(body)))
	}
}

// sigInput is one request to sign with AWS Signature Version 4.
type sigInput struct {
	Method      string
	URI         string            // the escaped path, no query
	Headers     map[string]string // lower-case name -> value; must include "host"
	PayloadHash string
	AmzDate     string // amzDateLayout
	Region      string
	KeyID       string
	Secret      string
}

// sigV4Authorization returns the Authorization header value (AWS Signature Version 4, service s3).
func sigV4Authorization(in sigInput) string {
	names := make([]string, 0, len(in.Headers))
	for n := range in.Headers {
		names = append(names, n)
	}
	sort.Strings(names)
	var canonHeaders strings.Builder
	for _, n := range names {
		canonHeaders.WriteString(n + ":" + strings.TrimSpace(in.Headers[n]) + "\n")
	}
	signedHeaders := strings.Join(names, ";")
	canonical := strings.Join([]string{
		in.Method, in.URI, "", canonHeaders.String(), signedHeaders, in.PayloadHash,
	}, "\n")
	dateStamp := in.AmzDate[:amzDateStampLen]
	scope := dateStamp + "/" + in.Region + "/s3/aws4_request"
	sts := "AWS4-HMAC-SHA256\n" + in.AmzDate + "\n" + scope + "\n" + hexSHA256([]byte(canonical))
	kDate := hmacSHA256([]byte("AWS4"+in.Secret), dateStamp)
	kRegion := hmacSHA256(kDate, in.Region)
	kService := hmacSHA256(kRegion, "s3")
	kSigning := hmacSHA256(kService, "aws4_request")
	sig := hex.EncodeToString(hmacSHA256(kSigning, sts))
	return "AWS4-HMAC-SHA256 Credential=" + in.KeyID + "/" + scope +
		", SignedHeaders=" + signedHeaders + ", Signature=" + sig
}

// signedHeaderNames are the request headers signV4 signs when they are set, besides host: the preconditions and the
// Cache-Control the bucket stores, so a proxy cannot change them without breaking the signature, and the two x-amz headers
// SigV4 requires.
var signedHeaderNames = []string{cacheControlName, ifMatchHeader, ifNoneMatchHeader, "X-Amz-Content-Sha256", "X-Amz-Date"}

// signV4 signs a request for S3 with an unsigned payload.
func signV4(req *http.Request, cfg config, now time.Time) {
	amzDate := now.UTC().Format(amzDateLayout)
	req.Header.Set("X-Amz-Date", amzDate)
	req.Header.Set("X-Amz-Content-Sha256", unsignedPayload)
	headers := map[string]string{"host": req.URL.Host}
	for _, name := range signedHeaderNames {
		if v := req.Header.Get(name); v != "" {
			headers[strings.ToLower(name)] = v
		}
	}
	req.Header.Set("Authorization", sigV4Authorization(sigInput{
		Method:      req.Method,
		URI:         req.URL.EscapedPath(),
		Headers:     headers,
		PayloadHash: unsignedPayload,
		AmzDate:     amzDate,
		Region:      cfg.Region,
		KeyID:       cfg.AccessKeyID,
		Secret:      cfg.SecretKey,
	}))
}

func hexSHA256(b []byte) string {
	s := sha256.Sum256(b)
	return hex.EncodeToString(s[:])
}

func hmacSHA256(key []byte, msg string) []byte {
	m := hmac.New(sha256.New, key)
	m.Write([]byte(msg))
	return m.Sum(nil)
}

func main() {
	dir := flag.String("dir", "", "extracted set: blobs/sha256/* and manifest.json.zst")
	dry := flag.Bool("dry-run", false, "count the objects and bytes that would be sent; no network, no credential")
	replaceLive := flag.String("replace-live", "", "replace the live manifest v1/manifest.json.zst, only while it holds this sha256 (its recovery copy must exist)")
	workers := flag.Int("workers", defaultWorkers, "parallel objects")
	flag.Parse()
	logger := slog.New(slog.NewJSONHandler(os.Stderr, &slog.HandlerOptions{Level: logLevelDefault}))
	fail := func(err error) {
		logger.Error("cargo-publish refused", "error", err.Error())
		fmt.Fprintln(os.Stderr, "cargo-publish:", err)
		os.Exit(1)
	}
	if *dir == "" {
		fail(errors.New("-dir is required"))
	}
	prefix := strings.TrimSpace(os.Getenv("NEVR_CARGO_PREFIX"))
	if prefix == "" {
		fail(errors.New("missing environment variable(s): NEVR_CARGO_PREFIX"))
	}
	if *replaceLive != "" {
		if err := checkReplaceLive(prefix, *replaceLive); err != nil {
			fail(err)
		}
	}
	p, err := buildPlan(*dir, prefix)
	if err != nil {
		fail(err)
	}
	if *dry {
		sum := dryRunSummary(p)
		sum.Prefix = prefix
		logger.Info("dry run", "objects", sum.Objects, "bytes", sum.Bytes, "blobs", sum.Blobs, "prefix", prefix)
		emit(sum)
		return
	}
	cfg, err := loadConfig(os.Getenv)
	if err != nil {
		fail(err)
	}
	cfg.Workers = *workers
	cfg.ReplaceLive = *replaceLive
	sum, err := runPublish(context.Background(), cfg, p, logger)
	emit(sum)
	if err != nil {
		fail(err)
	}
}

func emit(s summary) {
	var b bytes.Buffer
	enc := json.NewEncoder(&b)
	if err := enc.Encode(s); err != nil {
		fmt.Fprintln(os.Stderr, "cargo-publish: summary:", err)
		os.Exit(1)
	}
	os.Stdout.Write(b.Bytes())
}
