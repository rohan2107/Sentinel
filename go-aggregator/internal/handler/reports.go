// Package handler provides the HTTP handler for the POST /reports endpoint.
// Full dedup + storage will be wired in the next commit; this stub validates
// the request shape and returns the correct status codes so the C++ agent's
// retry logic behaves correctly from day one.
package handler

import (
	"encoding/json"
	"log/slog"
	"net/http"
	"time"
)

// reportRequest mirrors the JSON payload the C++ agent sends:
//
//	{"report": {...}, "hash": "<sha256-hex>"}
//
// report is kept as RawMessage so we can forward it without re-marshalling.
type reportRequest struct {
	Report json.RawMessage `json:"report"`
	Hash   string          `json:"hash"`
}

// Handler handles POST /reports. Implements http.Handler so it can be
// replaced with a richer version (with dedup cache + storage) in the next
// commit without touching main.go.
type Handler struct{}

// New returns a Handler ready to serve requests.
func New() *Handler { return &Handler{} }

func (h *Handler) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	start := time.Now()

	var req reportRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		slog.Warn("reports: malformed request body", "err", err, "remote", r.RemoteAddr)
		writeJSON(w, http.StatusBadRequest, `{"error":"invalid JSON body"}`)
		return
	}

	if req.Hash == "" {
		slog.Warn("reports: missing hash field", "remote", r.RemoteAddr)
		writeJSON(w, http.StatusBadRequest, `{"error":"missing hash"}`)
		return
	}

	if len(req.Report) == 0 {
		slog.Warn("reports: empty report field", "hash", req.Hash, "remote", r.RemoteAddr)
		writeJSON(w, http.StatusBadRequest, `{"error":"missing report"}`)
		return
	}

	// Stub acceptance — dedup + storage wired in next commit.
	slog.Info("reports: accepted (stub)", "hash", req.Hash, "latency_ms", time.Since(start).Milliseconds())
	writeJSON(w, http.StatusOK, `{"status":"accepted","hash":"`+req.Hash+`"}`)
}

// writeJSON sets Content-Type and writes a pre-formed JSON body.
// Using a pre-formed string avoids an extra json.Marshal allocation for
// simple fixed responses.
func writeJSON(w http.ResponseWriter, code int, body string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	w.Write([]byte(body)) //nolint:errcheck // response write errors are not actionable
}
