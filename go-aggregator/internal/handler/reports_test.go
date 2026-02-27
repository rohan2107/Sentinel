package handler

import (
	"bytes"
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/rohan2107/sentinel/go-aggregator/internal/dedup"
)

// reportHash computes the canonical SHA-256 hash for a JSON report string,
// mirroring the verification logic in ServeHTTP.
func reportHash(t *testing.T, reportJSON string) string {
	t.Helper()
	var m map[string]any
	if err := json.Unmarshal([]byte(reportJSON), &m); err != nil {
		t.Fatalf("reportHash: invalid JSON: %v", err)
	}
	canonical, _ := json.Marshal(m)
	return fmt.Sprintf("%x", sha256.Sum256(canonical))
}

// newHandler returns a Handler with a fresh dedup cache sized for tests.
func newHandler(t *testing.T) *Handler {
	t.Helper()
	cache, err := dedup.New(100)
	if err != nil {
		t.Fatalf("dedup.New: %v", err)
	}
	return New(cache)
}

func TestHandleReports_ValidationErrors(t *testing.T) {
	tests := []struct {
		name     string
		body     string
		wantCode int
	}{
		{
			name:     "missing hash rejected",
			body:     `{"report":{"score":75}}`,
			wantCode: http.StatusBadRequest,
		},
		{
			name:     "empty hash rejected",
			body:     `{"report":{"score":75},"hash":""}`,
			wantCode: http.StatusBadRequest,
		},
		{
			name:     "missing report field rejected",
			body:     `{"hash":"abc123"}`,
			wantCode: http.StatusBadRequest,
		},
		{
			name:     "malformed JSON rejected",
			body:     `not json at all`,
			wantCode: http.StatusBadRequest,
		},
		{
			name:     "empty body rejected",
			body:     ``,
			wantCode: http.StatusBadRequest,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			h := newHandler(t)
			req := httptest.NewRequest(http.MethodPost, "/reports", bytes.NewBufferString(tt.body))
			req.Header.Set("Content-Type", "application/json")
			rec := httptest.NewRecorder()

			h.ServeHTTP(rec, req)

			if rec.Code != tt.wantCode {
				t.Errorf("got status %d, want %d (body: %s)", rec.Code, tt.wantCode, rec.Body.String())
			}
		})
	}
}

func TestHandleReports_HashMismatch(t *testing.T) {
	h := newHandler(t)
	// 64-char hex string that does not match the report's real SHA-256.
	body := `{"report":{"score":75,"policy":"baseline"},"hash":"badhash000000000000000000000000000000000000000000000000000000000000"}`
	req := httptest.NewRequest(http.MethodPost, "/reports", strings.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	rec := httptest.NewRecorder()

	h.ServeHTTP(rec, req)

	if rec.Code != http.StatusBadRequest {
		t.Errorf("got status %d, want 400 (body: %s)", rec.Code, rec.Body.String())
	}
}

func TestHandleReports_ValidReport(t *testing.T) {
	h := newHandler(t)
	reportJSON := `{"score":75,"policy":"baseline"}`
	hash := reportHash(t, reportJSON)
	body := fmt.Sprintf(`{"report":%s,"hash":"%s"}`, reportJSON, hash)

	req := httptest.NewRequest(http.MethodPost, "/reports", strings.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	rec := httptest.NewRecorder()

	h.ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Errorf("got status %d, want 200 (body: %s)", rec.Code, rec.Body.String())
	}
}

func TestHandleReports_Duplicate(t *testing.T) {
	// Both requests share the same handler instance and therefore the same cache.
	h := newHandler(t)
	reportJSON := `{"score":75,"policy":"baseline"}`
	hash := reportHash(t, reportJSON)
	body := fmt.Sprintf(`{"report":%s,"hash":"%s"}`, reportJSON, hash)

	send := func() int {
		req := httptest.NewRequest(http.MethodPost, "/reports", strings.NewReader(body))
		req.Header.Set("Content-Type", "application/json")
		rec := httptest.NewRecorder()
		h.ServeHTTP(rec, req)
		return rec.Code
	}

	if code := send(); code != http.StatusOK {
		t.Fatalf("first request: got %d, want 200", code)
	}
	if code := send(); code != http.StatusConflict {
		t.Fatalf("second request (duplicate): got %d, want 409", code)
	}
}
