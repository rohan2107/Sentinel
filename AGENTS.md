# Sentinel: agent instructions

Host security-compliance agent (C++17): osquery + Lua rules → score → SQLite →
HTTP delivery to a Python or Go receiver. Before changing behaviour, read
`architecture/README.md`, especially **Known gaps**, which lists what the code
does *not* do. `docs/ROADMAP.md` has what's next.

## Commands

- Everything (both configs, both C++ test binaries, policy JSON, hash contract,
  backend): `./scripts/test.sh --config Both` (Windows: `.\scripts\test.ps1`)
- One test binary: `ctest --preset macos-release -R lua_evaluator` (or
  `delivery_foundation`)
- After any change to hashing or JSON encoding:
  `cmake --build --preset macos-release --target canon_dump && python3 tests/canonicalization/compare.py`
  (needs Go on PATH)
- Go, from `go-aggregator/`. CI runs `go mod tidy` (must leave no diff),
  `go vet`, `go test -race -count=1 ./...`, golangci-lint and govulncheck.
- `./scripts/run.sh` runs the agent. osquery is installed neither on the dev
  machine nor in CI, so every rule fails collection and the score is 0. That is
  expected, not a bug, and the tests are written not to need it.

## Things that have already bitten this repo

Each of these was a real bug that was found late.

- **Never use `assert()` in tests.** Release builds define `NDEBUG`, Release is
  the default and is what CI runs, so `assert()` compiles to nothing and the
  test prints `[PASS]` regardless. Use `SENTINEL_ASSERT`
  (`tests/test_delivery_foundation.cpp`) or `check()`
  (`tests/test_lua_evaluator.cpp`).
- **A new test isn't evidence until you've watched it fail.** Revert the fix,
  confirm the test fails, restore it, and do this in Release.
- **Docs must match code.** Earlier docs claimed a Lua timeout, enforced foreign
  keys and a scoring formula the code didn't have. Don't add a capability claim
  you haven't verified. When a Known gap is fixed, delete its entry and
  renumber. Keep one authoritative copy of each fact (`architecture/README.md`)
  and link to it instead of restating it.
- **`report_hash` is a three-language wire contract.** The C++ agent, Python
  backend and Go aggregator must produce byte-identical canonical JSON.
  Changing what it covers or how it encodes means changing all three and
  reviewing `golden.json` deliberately. `posture_hash` is agent-local and never
  sent.
- **Lua is pinned to 5.4** on every platform, because policy rules are Lua source
  shipped as data. Homebrew's plain `lua` is 5.5 and its headers shadow 5.4's;
  the CMake ABI probe exists for that, so don't "fix" a Lua version warning by
  switching versions. `vcpkg.json` pins `builtin-baseline` and overrides `lua`;
  bump them deliberately and watch Windows CI.
- **Required CI checks must not be path-filtered.** A required workflow that
  doesn't trigger never reports a status, so the PR is blocked forever. All
  required workflows trigger unconditionally; keep it that way.
- **Windows can't be run locally.** The maintainer develops on macOS, so
  `build-windows` is the only check on `#ifdef _WIN32` code, CMake's `WIN32`
  branches and `vcpkg.json`. Keep changes there minimal and read the CI log
  rather than guessing.

## Design constraints

- Policies are per-platform and rule ids are free-form strings. Never hardcode a
  rule id in schema or code. That is why `rule_results` is one row per rule.
- Schema changes are additive in `DB::init_schema()`, with `ALTER TABLE` guarded
  by `PRAGMA table_info`. The one exception so far is dropping the obsolete
  `features` table. There is no migration framework.
- Lua runs under a 1s CPU bound with only base/table/string/math opened. That is
  a resource bound, not a security boundary: policy authorship is trusted. Never
  describe it as making untrusted Lua safe.
- Running the agent or tests leaves gitignored files in the working directory
  (`sentinel_data.sqlite3*`, `reports/`, `backend/backend.db`). Clean them up.
  C++ tests use `remove_db()` so WAL sidecars go too.

## C++ conventions

- `std::unique_ptr` and RAII for ownership; no raw `new`/`delete` in new code.
- `sqlite3_finalize(stmt)` before any `throw`. Null-check `sqlite3_column_text`.
- Error messages say what failed and include `sqlite3_errmsg()` where it applies.
- Comments explain *why*, not *what*: state transitions, resource bounds and
  defensive checks especially.

## Git

- Branch first. Never commit to `master` and never push to it; a hook in
  `.claude/hooks/` enforces this for Claude Code, and branch protection enforces
  the push side.
- One concern per commit, each building and passing on its own. PRs keep their
  commits (no squash) so history stays bisectable.
- Message: `type: summary` (`fix`, `feat`, `docs`, `test`, `ci`, `build`,
  `chore`), then a body covering why, what was found, and how it was verified.
- No `Co-Authored-By` or "Generated with" trailers; commits carry only the
  maintainer's authorship.
