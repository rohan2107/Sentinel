#!/usr/bin/env python3
"""
Emits the canonical encoding and SHA-256 of every fixture, one JSON object per
line, for compare.py to diff against the C++ and Go emitters.

Imports backend/canonical.py, the same module backend/server.py verifies with,
so this exercises the backend's real encoder rather than a copy. That module
deliberately has no FastAPI or database imports, so this needs no dependencies
beyond the standard library.

Usage: canon_dump.py <fixtures.json>
"""
import json
import pathlib
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "backend"))

from canonical import canonical_json, compute_hash  # noqa: E402


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: canon_dump.py <fixtures.json>", file=sys.stderr)
        return 2

    fixtures = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
    if not isinstance(fixtures, list):
        print("fixtures must be a JSON array", file=sys.stderr)
        return 1

    for f in fixtures:
        line = {"id": f["id"], "impl": "python"}
        try:
            line["canonical"] = canonical_json(f["report"])
            line["hash"] = compute_hash(f["report"])
        except Exception as e:  # noqa: BLE001 - reported, not handled
            line["error"] = f"{type(e).__name__}: {e}"
        # ensure_ascii=True here: this is the transport for the comparison, not
        # the artifact being compared, and ASCII output keeps it terminal-safe.
        print(json.dumps(line, ensure_ascii=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
