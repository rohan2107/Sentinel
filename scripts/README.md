# Build scripts

Four operations, two script families that behave the same way: `.ps1` for
Windows, `.sh` for macOS and Linux. Both are thin wrappers over CMake presets, so
anything they do can be done directly with `cmake --preset`.

| Operation | Windows | macOS / Linux |
|---|---|---|
| Build | `.\scripts\build.ps1 -Config Debug` | `./scripts/build.sh --config Debug` |
| Run the agent | `.\scripts\run.ps1 -Policy policies\sample_policy.json` | `./scripts/run.sh --policy policies/macos_policy.json` |
| Test | `.\scripts\test.ps1 -Config Both` | `./scripts/test.sh --config Both` |
| Smoketest | `.\scripts\smoketest.ps1` | `./scripts/smoketest.sh` |

Every `.sh` script takes `--help`.

## Differences worth knowing

**Default configuration.** The shell scripts default to `Release`; `build.ps1`
defaults to `Debug`. Release is what CI validates and it lands in `build/`, the
path `run.sh` looks in. Debug goes to `build-debug/`, because single-config
generators cannot share one build directory between configurations the way the
Visual Studio generator does.

**Policy selection.** `run.sh` picks the policy matching the host OS when
`--policy` is omitted — policies are not portable, since each rule carries an
osquery query and the tables differ per platform.

**Forwarding arguments.** Anything after `--` goes to the agent:

```bash
./scripts/run.sh -- --enable-delivery --backend-url http://localhost:8000
```

**What `test.sh` covers.** Build, both test binaries via ctest, policy JSON
validation, the cross-language canonicalization test, and the backend syntax
check — the same set as CI. Steps whose toolchain is missing are skipped with a
reason rather than silently passing.

## Prerequisites

Windows: Visual Studio 2022 build tools, vcpkg with the packages in
`vcpkg.json`, PowerShell 7+.

macOS: `brew install cmake lua@5.4 spdlog nlohmann-json sol2 cpp-httplib`.

Both: osquery is a **runtime** dependency of the agent only. Without it the agent
still builds and runs, but every rule fails collection and the score reads 0 for
that reason rather than because the host is misconfigured. Neither test binary
needs it.
