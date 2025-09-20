# Sentinel

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

**Sentinel** is a lightweight C++ agent that evaluates device security using [osquery](https://osquery.io) and [Lua](https://www.lua.org/) rules and then produces a score.  
It reads JSON policy files, runs the associated osquery checks, evaluates them via Lua, and generates a compliance score with detailed reporting.

## Features
- 🚀 Runs osquery queries defined in policy JSON
- 📜 Evaluates results against Lua rules
- 📊 Produces a compliance score
- 📝 Outputs JSON reports to disk and persists results in SQLite
- 🖥️ Works on Windows (tested on VS2022 + vcpkg)

## Getting Started

### Prerequisites
- **Visual Studio 2022** with C++ build tools
- **[vcpkg](https://github.com/microsoft/vcpkg)** with the following packages installed:
  - `nlohmann-json`
  - `spdlog`
  - `sol2`
  - `lua`
  - `sqlite3`
- **[osquery](https://osquery.io/downloads/official)** installed and `osqueryi.exe` available in `PATH`

### Build
From repo root:

```bat
scripts\build.bat
```

### Run
```bat
scripts\build.bat
```