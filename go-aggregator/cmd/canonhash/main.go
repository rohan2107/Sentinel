// Command canonhash emits the canonical encoding and SHA-256 of every fixture,
// one JSON object per line, for tests/canonicalization/compare.py to diff
// against the C++ and Python emitters.
//
// It calls internal/canonical directly, so this exercises the aggregator's real
// encoder rather than a copy.
//
// Usage: canonhash <fixtures.json>
package main

import (
	"encoding/json"
	"fmt"
	"os"

	"github.com/rohan2107/sentinel/go-aggregator/internal/canonical"
)

type fixture struct {
	ID     string         `json:"id"`
	Report map[string]any `json:"report"`
}

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: canonhash <fixtures.json>")
		os.Exit(2)
	}

	raw, err := os.ReadFile(os.Args[1])
	if err != nil {
		fmt.Fprintf(os.Stderr, "cannot read fixtures: %v\n", err)
		os.Exit(1)
	}

	var fixtures []fixture
	if err := json.Unmarshal(raw, &fixtures); err != nil {
		fmt.Fprintf(os.Stderr, "invalid fixtures JSON: %v\n", err)
		os.Exit(1)
	}

	enc := json.NewEncoder(os.Stdout)
	for _, f := range fixtures {
		line := map[string]any{"id": f.ID, "impl": "go"}
		// Both JSON and Hash are called rather than hashing the bytes here, so
		// the pair is exercised as the handler uses them. Encoding twice is
		// irrelevant at fixture scale and keeps this tool free of its own copy
		// of the hashing step.
		b, err := canonical.JSON(f.Report)
		if err == nil {
			var h string
			if h, err = canonical.Hash(f.Report); err == nil {
				line["canonical"] = string(b)
				line["hash"] = h
			}
		}
		if err != nil {
			line["error"] = err.Error()
		}
		// Escaping is left on here: this is the transport for the comparison,
		// not the artifact being compared, and it keeps output terminal-safe.
		if err := enc.Encode(line); err != nil {
			fmt.Fprintf(os.Stderr, "encode failed: %v\n", err)
			os.Exit(1)
		}
	}
}
