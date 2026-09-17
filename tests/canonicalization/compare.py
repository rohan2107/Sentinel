#!/usr/bin/env python3
"""
Differential test for the report-hash canonicalization contract.

The C++ agent computes a report hash; the Python backend and the Go aggregator
each recompute it to verify. All three must produce byte-identical canonical
JSON or every report is rejected as a hash mismatch. Three independent encoders
(nlohmann::json::dump, json.dumps, encoding/json) agreeing is not automatic:
they differ by default on non-ASCII escaping, HTML escaping, and number
formatting.

This runs all three over tests/canonicalization/fixtures.json and fails if any
fixture does not agree across every available implementation.

Usage:
  compare.py [--cpp PATH] [--go PATH] [--update-golden] [--allow-missing]

Exit codes: 0 all agree, 1 disagreement or missing golden, 2 usage/setup error.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import shutil
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]
FIXTURES = HERE / "fixtures.json"
GOLDEN = HERE / "golden.json"


def run_emitter(name: str, cmd: list[str]) -> dict[str, dict]:
    """Run one emitter and return {fixture_id: record}."""
    try:
        proc = subprocess.run(
            cmd, capture_output=True, text=True, encoding="utf-8", cwd=REPO_ROOT
        )
    except OSError as e:
        raise RuntimeError(f"{name}: could not execute {cmd[0]}: {e}") from e

    if proc.returncode != 0:
        raise RuntimeError(
            f"{name}: exited {proc.returncode}\n"
            f"  cmd: {' '.join(cmd)}\n"
            f"  stderr: {proc.stderr.strip()}"
        )

    out: dict[str, dict] = {}
    for lineno, line in enumerate(proc.stdout.splitlines(), start=1):
        line = line.strip()
        if not line:
            continue
        try:
            rec = json.loads(line)
        except json.JSONDecodeError as e:
            raise RuntimeError(f"{name}: line {lineno} is not JSON: {e}") from e
        out[rec["id"]] = rec
    return out


def describe(rec: dict | None) -> str:
    if rec is None:
        return "<absent>"
    if "error" in rec:
        return f"<error: {rec['error']}>"
    return rec.get("hash", "<no hash>")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cpp", help="path to the canon_dump binary")
    ap.add_argument("--go", help="path to the canonhash binary")
    ap.add_argument(
        "--update-golden",
        action="store_true",
        help="rewrite golden.json from the agreed hashes (review the diff)",
    )
    ap.add_argument(
        "--allow-missing",
        action="store_true",
        help="skip implementations whose toolchain is unavailable instead of failing",
    )
    args = ap.parse_args()

    if not FIXTURES.is_file():
        print(f"ERROR: fixtures not found at {FIXTURES}", file=sys.stderr)
        return 2

    # --- Resolve the three emitters ----------------------------------------
    emitters: dict[str, list[str]] = {}
    missing: list[str] = []

    cpp = args.cpp or next(
        (
            str(p)
            for p in (
                REPO_ROOT / "build" / "canon_dump",
                REPO_ROOT / "build-debug" / "canon_dump",
                REPO_ROOT / "build" / "Release" / "canon_dump.exe",
                REPO_ROOT / "build" / "Debug" / "canon_dump.exe",
            )
            if p.is_file()
        ),
        None,
    )
    if cpp:
        emitters["cpp"] = [cpp, str(FIXTURES)]
    else:
        missing.append("cpp (build the canon_dump target)")

    emitters["python"] = [sys.executable, str(HERE / "canon_dump.py"), str(FIXTURES)]

    if args.go:
        emitters["go"] = [args.go, str(FIXTURES)]
    elif shutil.which("go"):
        emitters["go"] = [
            "go",
            "run",
            "./cmd/canonhash",
            str(FIXTURES),
        ]
    else:
        missing.append("go (go toolchain not on PATH)")

    if missing and not args.allow_missing:
        for m in missing:
            print(f"ERROR: missing implementation: {m}", file=sys.stderr)
        print(
            "\nA two-way comparison can hide a divergence, so this is an error "
            "by default.\nPass --allow-missing to run with what is available.",
            file=sys.stderr,
        )
        return 2
    for m in missing:
        print(f"WARNING: skipping {m}")

    # --- Run them -----------------------------------------------------------
    results: dict[str, dict[str, dict]] = {}
    for name, cmd in emitters.items():
        # The Go emitter must run from the module directory.
        cwd_note = ""
        if name == "go" and cmd[0] == "go":
            cwd_note = " (in go-aggregator/)"
        print(f"running {name}{cwd_note}...")
        try:
            if name == "go" and cmd[0] == "go":
                proc = subprocess.run(
                    cmd,
                    capture_output=True,
                    text=True,
                    encoding="utf-8",
                    cwd=REPO_ROOT / "go-aggregator",
                )
                if proc.returncode != 0:
                    raise RuntimeError(
                        f"go: exited {proc.returncode}\n  stderr: {proc.stderr.strip()}"
                    )
                recs = {}
                for line in proc.stdout.splitlines():
                    if line.strip():
                        r = json.loads(line)
                        recs[r["id"]] = r
                results[name] = recs
            else:
                results[name] = run_emitter(name, cmd)
        except RuntimeError as e:
            print(f"ERROR: {e}", file=sys.stderr)
            return 2

    fixtures = json.loads(FIXTURES.read_text(encoding="utf-8"))
    impls = list(results)

    # --- Compare ------------------------------------------------------------
    agreed: dict[str, str] = {}
    failures: list[str] = []

    print()
    print(f"{'fixture':<30} {'result':<12} detail")
    print("-" * 78)

    for f in fixtures:
        fid = f["id"]
        # expect="diverge" marks input outside the supported wire contract, where
        # the three encoders are known not to agree. Asserting the divergence
        # still holds keeps it documented: if someone later makes these agree,
        # this fails and tells them to reclassify the fixture rather than
        # letting a tolerated case quietly become a passing one.
        expect = f.get("expect", "agree")
        recs = {impl: results[impl].get(fid) for impl in impls}

        errored = {i: r for i, r in recs.items() if r is None or "error" in r}
        hashes = {i: r["hash"] for i, r in recs.items() if r and "error" not in r}
        distinct = set(hashes.values())
        show_forms = False

        if errored:
            status = "ERROR"
            detail = ", ".join(f"{i}={describe(r)}" for i, r in errored.items())
            failures.append(fid)
        elif expect == "diverge":
            if len(distinct) > 1:
                status = "diverge-ok"
                detail = "known unsupported input; divergence documented"
            else:
                status = "UNEXPECTED"
                detail = (
                    "fixture is marked expect=diverge but all implementations "
                    "now agree - reclassify it as expect=agree"
                )
                failures.append(fid)
        elif len(distinct) == 1:
            status = "agree"
            detail = next(iter(distinct))[:16] + "..."
            agreed[fid] = next(iter(distinct))
        else:
            status = "DIVERGE"
            detail = ", ".join(f"{i}={h[:12]}..." for i, h in sorted(hashes.items()))
            failures.append(fid)
            show_forms = True

        print(f"{fid:<30} {status:<12} {detail}")

        if show_forms:
            # Show the canonical forms so the cause is visible, not guessed at.
            for impl in sorted(hashes):
                print(f"{'':<30} {impl:>8}: {recs[impl]['canonical']!r}")

    # --- Golden file --------------------------------------------------------
    print()
    if args.update_golden:
        GOLDEN.write_text(
            json.dumps(
                {"_note": "Agreed canonical hashes. Regenerate with --update-golden.",
                 "hashes": agreed},
                indent=2,
                sort_keys=True,
            )
            + "\n",
            encoding="utf-8",
        )
        print(f"wrote {GOLDEN.relative_to(REPO_ROOT)} ({len(agreed)} hashes)")
    elif GOLDEN.is_file():
        golden = json.loads(GOLDEN.read_text(encoding="utf-8")).get("hashes", {})
        drifted = [
            f"{fid}: golden={golden[fid][:12]}... now={h[:12]}..."
            for fid, h in agreed.items()
            if fid in golden and golden[fid] != h
        ]
        untracked = sorted(set(agreed) - set(golden))
        if drifted:
            print("GOLDEN DRIFT (all implementations changed together):")
            for d in drifted:
                print(f"  {d}")
            failures.extend(f.split(":")[0] for f in drifted)
        if untracked:
            print(f"note: {len(untracked)} fixture(s) not yet in golden.json: "
                  f"{', '.join(untracked)}")
        if not drifted:
            print(f"golden.json: {len(agreed) - len(untracked)} hash(es) unchanged")
    else:
        print("note: no golden.json yet; create one with --update-golden")

    # --- Verdict ------------------------------------------------------------
    print()
    if failures:
        print(f"FAILED: {len(set(failures))} fixture(s) did not agree: "
              f"{', '.join(sorted(set(failures)))}")
        return 1
    print(f"OK: {len(agreed)} fixture(s) agree across {', '.join(impls)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
