// Package handler provides the HTTP handler for the POST /reports endpoint.
// Each request is validated, hash-verified, and deduplicated before acceptance.
package handler

import (
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"log/slog"
	"net/http"
	"time"

	"github.com/rohan2107/sentinel/go-aggregator/internal/dedup"
)

// reportRequest mirrors the JSON payload the C++ agent sends:
//
//	{"report": {...}, "hash": "<sha256-hex>"}
//
// report is kept as RawMessage so it can be canonicalised without double-parsing.
type reportRequest struct {
	Report json.RawMessage `json:"report"`
	Hash   string          `json:"hash"`
}

// Handler handles POST /reports. Implements http.Handler.
type Handler struct {
	dedup *dedup.SeenCache
}

// New returns a Handler that uses cache for in-memory deduplication.
func New(cache *dedup.SeenCache) *Handler {
	return &Handler{dedup: cache}
}

func (h *Handler) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	start := time.Now()

	var req reportRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		slog.Warn("reports: malformed request body", "err", err, "remote", r.RemoteAddr)
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "invalid JSON body"})
		return
	}

	if req.Hash == "" {
		slog.Warn("reports: missing hash field", "remote", r.RemoteAddr)
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "missing hash"})
		return
	}

	if len(req.Report) == 0 {
		slog.Warn("reports: empty report field", "hash", req.Hash, "remote", r.RemoteAddr)
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "missing report"})
		return
	}

	// Verify hash: unmarshal report into a map so encoding/json re-marshals it
	// with sorted keys (canonical form), then compare SHA-256 against req.Hash.
	// This matches the C++ agent's nlohmann/json canonical serialisation.
	var m map[string]any
	if err := json.Unmarshal(req.Report, &m); err != nil {
		slog.Warn("reports: report field is not a JSON object", "err", err, "remote", r.RemoteAddr)
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "report must be a JSON object"})
		return
	}
	canonical, _ := json.Marshal(m)
	computed := fmt.Sprintf("%x", sha256.Sum256(canonical))
	if computed != req.Hash {
		slog.Warn("reports: hash mismatch", "remote", r.RemoteAddr, "want", req.Hash, "got", computed)
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "hash mismatch"})
		return
	}

	// Dedup: return 409 if this hash was processed recently.
	if h.dedup.Contains(req.Hash) {
		slog.Info("reports: duplicate (cache hit)", "hash", req.Hash)
		writeJSON(w, http.StatusConflict, map[string]string{"status": "duplicate", "hash": req.Hash})
		return
	}
	h.dedup.Add(req.Hash)

	slog.Info("reports: accepted", "hash", req.Hash, "latency_ms", time.Since(start).Milliseconds())
	writeJSON(w, http.StatusOK, map[string]string{"status": "accepted", "hash": req.Hash})
}

// writeJSON encodes v as JSON, sets Content-Type, and writes the given status code.
func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	if err := json.NewEncoder(w).Encode(v); err != nil {
		slog.Warn("reports: failed to write response", "err", err)
	}
}
