// Package canonical produces the deterministic JSON encoding that report
// hashes are computed over.
//
// This is a wire-format contract, not an implementation detail: the C++ agent
// computes a hash and the aggregator recomputes it to verify. If the two
// encoders disagree on so much as one byte, every report is rejected as a hash
// mismatch. The same contract is implemented in src/report_hasher.cpp
// (nlohmann::json::dump) and backend/server.py (json.dumps), and
// tests/canonicalization checks all three against shared fixtures.
//
// It lives in its own package so that test tooling can exercise exactly the
// code the HTTP handler uses, rather than a copy that could drift from it.
package canonical

import (
	"bytes"
	"crypto/sha256"
	"encoding/json"
	"fmt"
)

// JSON encodes m in canonical form: object keys sorted, no insignificant
// whitespace, and HTML escaping disabled.
//
// encoding/json sorts map keys on its own, which is what makes this comparable
// to the other two implementations. SetEscapeHTML(false) is required because
// Go escapes <, > and & by default and neither nlohmann nor Python's json does.
func JSON(m map[string]any) ([]byte, error) {
	var buf bytes.Buffer
	enc := json.NewEncoder(&buf)
	enc.SetEscapeHTML(false)
	if err := enc.Encode(m); err != nil {
		return nil, err
	}
	// Encode appends a trailing newline; the hash is over the bare document.
	return bytes.TrimSuffix(buf.Bytes(), []byte{'\n'}), nil
}

// Hash returns the lowercase hex SHA-256 of the canonical encoding of m.
func Hash(m map[string]any) (string, error) {
	b, err := JSON(m)
	if err != nil {
		return "", err
	}
	return fmt.Sprintf("%x", sha256.Sum256(b)), nil
}
