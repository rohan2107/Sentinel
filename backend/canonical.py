"""
Deterministic JSON encoding that report hashes are computed over.

This is a wire-format contract, not an implementation detail: the C++ agent
computes a hash and this backend recomputes it to verify. If the two encoders
disagree on so much as one byte, every report is rejected as a hash mismatch.
The same contract is implemented in src/report_hasher.cpp
(nlohmann::json::dump) and go-aggregator/internal/canonical, and
tests/canonicalization checks all three against shared fixtures.

Kept in its own module, with no FastAPI or database imports, so the differential
test can exercise exactly the code the server uses without starting a web app or
touching a database on import.
"""
import hashlib
import json
from typing import Any

__all__ = ["canonical_json", "compute_hash"]


def canonical_json(report: dict[str, Any]) -> str:
    """
    Encode report in canonical form.

    sort_keys gives key ordering; separators strips insignificant whitespace;
    ensure_ascii=False is required because Python escapes non-ASCII into
    backslash-u escapes by default, and neither nlohmann nor Go's encoder does.
    Without it every report from a host whose name contains a non-ASCII
    character is rejected as a hash mismatch.
    """
    return json.dumps(
        report, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    )


def compute_hash(report: dict[str, Any]) -> str:
    """Lowercase hex SHA-256 of the canonical encoding of report."""
    return hashlib.sha256(canonical_json(report).encode("utf-8")).hexdigest()
