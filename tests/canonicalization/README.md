# Report hash canonicalization contract

The agent computes a SHA-256 over a report and sends both. Each receiver
recomputes that hash from the report it parsed and rejects the request if the two
disagree. So the byte-for-byte encoding used before hashing is a **wire-format
contract between three independently written encoders**:

| Implementation | Encoder | Used by |
|---|---|---|
| C++ | `nlohmann::json::dump(-1, ' ', false, strict)` | [`src/report_hasher.cpp`](../../src/report_hasher.cpp) — the agent, which produces the hash |
| Python | `json.dumps(sort_keys=True, separators=(",", ":"), ensure_ascii=False)` | [`backend/canonical.py`](../../backend/canonical.py), used by `server.py` — verifies it |
| Go | `json.Encoder` with `SetEscapeHTML(false)` | [`go-aggregator/internal/canonical`](../../go-aggregator/internal/canonical/canonical.go) — verifies it |

None of these agree by default. Left unchecked, a mismatch does not fail loudly:
it makes every affected report return `400 hash mismatch` forever, which looks
like an agent bug rather than an encoder disagreement.

## Running it

```bash
cmake --build --preset macos-release --target canon_dump
python3 tests/canonicalization/compare.py
```

Each of the three emitters calls its project's **real** canonicalizer rather than
a reimplementation — that is the point, and it is why the Go and Python versions
were extracted out of their HTTP handlers into importable functions. A test that
reimplemented the encoding would prove nothing.

Flags: `--update-golden` rewrites `golden.json`, `--allow-missing` runs with
whichever toolchains are present (off by default, because a two-way comparison
can hide a three-way disagreement), `--cpp` / `--go` point at prebuilt binaries.

Exit codes: `0` agreement, `1` disagreement or golden drift, `2` setup problem.

## What it catches

Two distinct failure modes:

1. **Divergence** — the implementations disagree with each other *now*. This is
   the one that breaks production.
2. **Golden drift** — all three changed *together*, so they still agree but no
   longer produce the hashes they used to. That silently invalidates every hash
   already stored by a receiver, and three-way comparison alone cannot see it.
   `golden.json` pins the absolute values.

## The contract

Supported in a report, and verified to encode identically in all three:

- **Objects** — keys sorted by codepoint; no whitespace; `{}` when empty
- **Strings** — raw UTF-8, *not* `\uXXXX` escapes; `<`, `>`, `&`, `/` unescaped;
  `"` `\` `\n` `\t` escaped the standard way
- **Booleans** and **null**
- **Arrays**, including empty, and nested objects
- **Integers** within ±(2^53 − 1)

Unicode is **not** normalised, which is deliberate: `café` in NFC and in NFD are
different byte sequences and must hash differently. Normalising would mean the
agent and receiver could disagree about whether two reports are the same.

### Outside the contract

Two fixtures are marked `"expect": "diverge"` and assert that the disagreement
*still exists*, so it stays documented rather than tolerated. If someone makes
them agree, the test fails and asks for the fixture to be reclassified.

**Integers beyond 2^53** (`09-integer-bounds`). Go's `encoding/json` decodes
every number into `float64` when the destination is `map[string]any`, so
`9007199254740993` silently becomes `...992`. C++ and Python preserve it.

**Floats** (`10-floats`). Go renders `1e20` as `100000000000000000000` and the
float `1.0` as `1`; C++ and Python render `1e+20` and `1.0`. Float formatting is
the least portable corner of JSON.

Neither is worth chasing: a report carries a `score` in 0–100 and boolean rule
outcomes, so it has no floats and no large integers. The correct fix is to
**declare the number range and validate it at the boundary**, so an unsupported
value is rejected with a clear error instead of silently producing a hash that
one side cannot reproduce. That validation is not implemented yet — it is the
known gap this file records.

## Why this exists

It was written as the safety net for splitting the report hash in two, which has
since landed. One hash used to serve both "has the posture changed?" and "is this
the same report?", and because it covers `timestamp` it was always unique, so
deduplication never suppressed anything.

The split kept this contract intact, which was the point of having the test
first: `report_hash` still covers the whole report, so it is still what receivers
recompute, and `golden.json` was unchanged by the work. The new `posture_hash`
covers `policy`, `score` and `details` only, and is agent-local — it never goes
on the wire, so it is outside this contract. The two hashes share one
canonicalizer, so anything that changes the encoding still shows up here.
