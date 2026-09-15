// Black-box contract tests for the crash-report ingest implementation contract.
// Standard library only: a sink written in any language can use this gate.
package crash_ingest

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"mime/multipart"
	"net/http"
	"net/url"
	"os"
	"strings"
	"testing"
	"time"
)

const urlEnv = "CRASH_INGEST_URL"
const tokenEnv = "CRASH_INGEST_TOKEN"
const maxQuestMinidumpBytes = 32 << 20

type acknowledgement struct {
	ReportID  string `json:"report_id"`
	Duplicate bool   `json:"duplicate"`
}

type filePart struct {
	name string
	body []byte
}
type formSpec struct {
	fields map[string]string
	files  map[string]filePart
	token  string
}

func endpoint(t *testing.T) (string, string) {
	t.Helper()
	raw, token := os.Getenv(urlEnv), os.Getenv(tokenEnv)
	if raw == "" || token == "" {
		t.Skipf("set %s and %s for an isolated staging sink", urlEnv, tokenEnv)
	}
	u, err := url.Parse(raw)
	if err != nil || u.Scheme == "" || u.Host == "" {
		t.Fatalf("%s must be absolute: %q", urlEnv, raw)
	}
	if u.Scheme != "https" && os.Getenv("CRASH_INGEST_ALLOW_HTTP_FOR_TESTS") != "1" {
		t.Fatal("refusing non-HTTPS endpoint; local tests require CRASH_INGEST_ALLOW_HTTP_FOR_TESTS=1")
	}
	return raw, token
}

func canonicalFields(platform, id string) map[string]string {
	return map[string]string{
		"schema_version": "1", "client_report_id": id, "platform": platform,
		"prod": "echovr", "ver": "contract-test", "captured_at": "2026-09-15T12:34:56Z",
		"crash_summary_text": "[NEVR.CRASH] contract-test=true\\n",
	}
}

func uniqueID(t *testing.T, suffix int) string {
	t.Helper()
	return fmt.Sprintf("b7e2c%03x-7f9a-4d12-8abc-%012x", time.Now().UnixNano()&0xfff, suffix)
}

func send(t *testing.T, target string, spec formSpec) (*http.Response, []byte) {
	t.Helper()
	var body bytes.Buffer
	w := multipart.NewWriter(&body)
	for key, value := range spec.fields {
		if err := w.WriteField(key, value); err != nil {
			t.Fatal(err)
		}
	}
	for field, file := range spec.files {
		part, err := w.CreateFormFile(field, file.name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := part.Write(file.body); err != nil {
			t.Fatal(err)
		}
	}
	if err := w.Close(); err != nil {
		t.Fatal(err)
	}
	req, err := http.NewRequest(http.MethodPost, target, &body)
	if err != nil {
		t.Fatal(err)
	}
	req.Header.Set("Content-Type", w.FormDataContentType())
	if spec.token != "" {
		req.Header.Set("Authorization", "Bearer "+spec.token)
	}
	client := &http.Client{Timeout: 15 * time.Second, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
	resp, err := client.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	payload, err := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if err != nil {
		t.Fatal(err)
	}
	return resp, payload
}

func requireAck(t *testing.T, resp *http.Response, body []byte, status int) acknowledgement {
	t.Helper()
	if resp.StatusCode != status {
		t.Fatalf("status=%d, want=%d body=%q", resp.StatusCode, status, body)
	}
	if !strings.HasPrefix(resp.Header.Get("Content-Type"), "application/json") {
		t.Fatalf("Content-Type=%q, want application/json", resp.Header.Get("Content-Type"))
	}
	var ack acknowledgement
	if err := json.Unmarshal(body, &ack); err != nil {
		t.Fatalf("invalid acknowledgement JSON %q: %v", body, err)
	}
	if ack.ReportID == "" || len(ack.ReportID) > 256 {
		t.Fatalf("invalid report_id %#v", ack.ReportID)
	}
	return ack
}

func TestContract_RejectsMissingOrWrongBearerToken(t *testing.T) {
	target, token := endpoint(t)
	for name, supplied := range map[string]string{"missing": "", "wrong": token + "-wrong"} {
		t.Run(name, func(t *testing.T) {
			resp, body := send(t, target, formSpec{fields: canonicalFields("windows", uniqueID(t, 1)), token: supplied})
			if resp.StatusCode != http.StatusUnauthorized && resp.StatusCode != http.StatusForbidden {
				t.Fatalf("status=%d, want 401 or 403 body=%q", resp.StatusCode, body)
			}
		})
	}
}

func TestContract_AcceptsWindowsTextOnly(t *testing.T) {
	target, token := endpoint(t)
	resp, body := send(t, target, formSpec{fields: canonicalFields("windows", uniqueID(t, 2)), token: token})
	if requireAck(t, resp, body, http.StatusCreated).Duplicate {
		t.Fatal("new report was marked duplicate")
	}
}

func TestContract_AcceptsQuestMinidumpAndMaps(t *testing.T) {
	target, token := endpoint(t)
	resp, body := send(t, target, formSpec{fields: canonicalFields("quest", uniqueID(t, 3)), token: token, files: map[string]filePart{
		"upload_file_minidump": {name: "contract.dmp", body: []byte("MDMP contract fixture")},
		"upload_file_maps":     {name: "contract.dmp.maps", body: []byte("maps fixture")},
	}})
	if requireAck(t, resp, body, http.StatusCreated).Duplicate {
		t.Fatal("new report was marked duplicate")
	}
}

func TestContract_DuplicateIsIdempotent(t *testing.T) {
	target, token := endpoint(t)
	fields := canonicalFields("windows", uniqueID(t, 4))
	firstResponse, firstBody := send(t, target, formSpec{fields: fields, token: token})
	first := requireAck(t, firstResponse, firstBody, http.StatusCreated)
	secondResponse, secondBody := send(t, target, formSpec{fields: fields, token: token})
	second := requireAck(t, secondResponse, secondBody, http.StatusOK)
	if !second.Duplicate || second.ReportID != first.ReportID {
		t.Fatalf("duplicate=%+v original=%+v", second, first)
	}
}

func TestContract_RejectsMalformedAndInconsistentRequests(t *testing.T) {
	target, token := endpoint(t)
	missing := canonicalFields("windows", uniqueID(t, 5))
	delete(missing, "crash_summary_text")
	unknown := canonicalFields("windows", uniqueID(t, 6))
	unknown["surprise"] = "no"
	cases := []struct {
		name string
		spec formSpec
	}{
		{"missing-summary", formSpec{fields: missing, token: token}},
		{"invalid-uuid", formSpec{fields: canonicalFields("windows", "not-a-uuid"), token: token}},
		{"unknown-field", formSpec{fields: unknown, token: token}},
		{"quest-without-minidump", formSpec{fields: canonicalFields("quest", uniqueID(t, 8)), token: token}},
		{"windows-minidump", formSpec{fields: canonicalFields("windows", uniqueID(t, 7)), files: map[string]filePart{"upload_file_minidump": {name: "wrong.dmp", body: []byte("x")}}, token: token}},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			resp, body := send(t, target, tc.spec)
			if resp.StatusCode != http.StatusBadRequest {
				t.Fatalf("status=%d, want=400 body=%q", resp.StatusCode, body)
			}
		})
	}
}

func TestContract_RejectsOversizedMinidump(t *testing.T) {
	target, token := endpoint(t)
	// This is one byte over the contractual file limit. Keep this an explicit
	// black-box test: accepting and then truncating a dump is data corruption.
	overdue := bytes.Repeat([]byte{'x'}, maxQuestMinidumpBytes+1)
	resp, body := send(t, target, formSpec{
		fields: canonicalFields("quest", uniqueID(t, 9)), token: token,
		files: map[string]filePart{"upload_file_minidump": {name: "too-large.dmp", body: overdue}},
	})
	if resp.StatusCode != http.StatusRequestEntityTooLarge {
		t.Fatalf("status=%d, want=413 body=%q", resp.StatusCode, body)
	}
}
