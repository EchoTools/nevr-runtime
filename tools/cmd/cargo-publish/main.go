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
// and v1/manifests/<sha256 of the frame>.json.zst. A plain manifest.json in the set is not read.
//
// Blobs are written at <prefix>blobs/sha256/<hex>. Per object: a HEAD on the public domain (200 skips the object, 404
// puts it, any other status is an error), a PUT with If-None-Match: * (the bucket refuses to overwrite), then a GET on the
// public domain whose SHA-256 must match. Nothing is deleted, nothing outside the prefix or the manifests directory is
// written, and no secret is logged. Dry run checks and hashes the set and prints the counts and bytes it would send,
// without the network and without any credential.
//
// Records are JSON on standard error (Info per uploaded object and for the summary, Debug per skipped object, Error for
// each failure); the run summary is one JSON line on standard output. Exit 1 names each failure.
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
		p.Objects = append(p.Objects, object{Key: prefix + blobsDir + name, Local: local, SHA: sum, Size: size})
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
	keys := []string{prefix + manifestFrameName, manifestsKeyDir + sum + manifestFrameExt}
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

// runPublish sends every object of the plan that is not already on the public domain. It keeps going past a failed
// object so every failure is named, then returns them joined.
func runPublish(ctx context.Context, cfg config, p plan, log *slog.Logger) (summary, error) {
	sum := summary{Objects: len(p.Objects), Bytes: p.Bytes, Blobs: p.Blobs, Prefix: cfg.Prefix}
	workers := cfg.Workers
	if workers < 1 {
		workers = 1
	}
	jobs := make(chan object)
	var mu sync.Mutex
	var errs []error
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
	for _, o := range p.Objects {
		if ctx.Err() != nil {
			break
		}
		jobs <- o
	}
	close(jobs)
	wg.Wait()
	log.Info("publish finished", "objects", sum.Objects, "uploaded", sum.Uploaded, "skipped_present", sum.Skipped,
		"verified", sum.Verified, "failures", len(errs), "prefix", cfg.Prefix)
	if err := ctx.Err(); err != nil {
		errs = append(errs, err)
	}
	return sum, errors.Join(errs...)
}

// publishOne returns "skipped" when the object is present, "uploaded" after a put that read back by hash.
func publishOne(ctx context.Context, cfg config, o object, log *slog.Logger) (string, error) {
	present, err := publicStatus(ctx, cfg, o.Key)
	if err != nil {
		log.Error("head failed", "key", o.Key, "error", err.Error())
		return "", fmt.Errorf("%s: head: %w", o.Key, err)
	}
	if present {
		log.Debug("object present, skipped", "key", o.Key, "bytes", o.Size)
		return "skipped", nil
	}
	if err := putObject(ctx, cfg, o); err != nil {
		log.Error("put failed", "key", o.Key, "error", err.Error())
		return "", fmt.Errorf("%s: put: %w", o.Key, err)
	}
	got, n, err := publicHash(ctx, cfg, o.Key)
	if err != nil {
		log.Error("readback failed", "key", o.Key, "error", err.Error())
		return "", fmt.Errorf("%s: readback: %w", o.Key, err)
	}
	if got != o.SHA || n != o.Size {
		log.Error("readback mismatch", "key", o.Key, "want_sha256", o.SHA, "got_sha256", got, "want_bytes", o.Size, "got_bytes", n)
		return "", fmt.Errorf("%s: readback mismatch: want sha256 %s and %d bytes, got %s and %d bytes", o.Key, o.SHA, o.Size, got, n)
	}
	log.Info("object uploaded", "key", o.Key, "bytes", o.Size, "sha256", o.SHA)
	return "uploaded", nil
}

func publicURL(cfg config, key string) string {
	return strings.TrimRight(cfg.PublicBase, "/") + "/" + key
}

// publicStatus reports whether the public domain serves the key: true on 200, false on 404, an error otherwise.
func publicStatus(ctx context.Context, cfg config, key string) (bool, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodHead, publicURL(cfg, key), nil)
	if err != nil {
		return false, err
	}
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

// publicHash fetches the key from the public domain and returns its SHA-256 and byte count.
func publicHash(ctx context.Context, cfg config, key string) (string, int64, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, publicURL(cfg, key), nil)
	if err != nil {
		return "", 0, err
	}
	resp, err := cfg.HTTP.Do(req)
	if err != nil {
		return "", 0, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return "", 0, fmt.Errorf("HTTP %d", resp.StatusCode)
	}
	h := sha256.New()
	n, err := io.Copy(h, resp.Body)
	if err != nil {
		return "", 0, err
	}
	return hex.EncodeToString(h.Sum(nil)), n, nil
}

// putObject writes the object with a conditional PUT: the bucket refuses (412) when the key already exists.
func putObject(ctx context.Context, cfg config, o object) error {
	f, err := os.Open(o.Local)
	if err != nil {
		return err
	}
	defer f.Close()
	u, err := url.Parse(strings.TrimRight(cfg.Endpoint, "/"))
	if err != nil {
		return fmt.Errorf("endpoint: %w", err)
	}
	u.Path = "/" + cfg.Bucket + "/" + o.Key
	req, err := http.NewRequestWithContext(ctx, http.MethodPut, u.String(), f)
	if err != nil {
		return err
	}
	req.ContentLength = o.Size
	req.Header.Set("If-None-Match", "*")
	req.Header.Set("Content-Type", "application/octet-stream")
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
		return fmt.Errorf("key already exists; not overwritten (HTTP 412)")
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

// signV4 signs a request for S3 with an unsigned payload.
func signV4(req *http.Request, cfg config, now time.Time) {
	amzDate := now.UTC().Format(amzDateLayout)
	req.Header.Set("X-Amz-Date", amzDate)
	req.Header.Set("X-Amz-Content-Sha256", unsignedPayload)
	headers := map[string]string{"host": req.URL.Host}
	for _, name := range []string{"If-None-Match", "X-Amz-Content-Sha256", "X-Amz-Date"} {
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
