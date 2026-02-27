package handler

import (
	"bytes"
	"net/http"
	"net/http/httptest"
	"testing"
)

func TestHandleReports(t *testing.T) {
	h := New()

	tests := []struct {
		name     string
		body     string
		wantCode int
	}{
		{
			name:     "valid report accepted",
			body:     `{"report":{"score":75,"policy":"baseline"},"hash":"abc123def456"}`,
			wantCode: http.StatusOK,
		},
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
			req := httptest.NewRequest(http.MethodPost, "/reports", bytes.NewBufferString(tt.body))
			req.Header.Set("Content-Type", "application/json")
			rec := httptest.NewRecorder()

			h.ServeHTTP(rec, req)

			if rec.Code != tt.wantCode {
				t.Errorf("got status %d, want %d (body: %s)", rec.Code, tt.wantCode, rec.Body.String())
			}
			t.Logf("[PASS] %s → HTTP %d", tt.name, rec.Code)
		})
	}
}
