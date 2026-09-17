cmake_minimum_required(VERSION 3.20)
project(Sentinel LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default to Release for single-config generators (Makefiles, Ninja) so a bare
# `cmake -S . -B build` on macOS/Linux doesn't silently produce an unoptimised
# binary. Multi-config generators (Visual Studio, Xcode) ignore this.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# vcpkg toolchain (Windows only, optional). Guarded by WIN32 so a stale
# C:/vcpkg path can never be selected when cross-compiling or on Unix.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND WIN32 AND EXISTS "C:/vcpkg/scripts/buildsystems/vcpkg.cmake")
  set(CMAKE_TOOLCHAIN_FILE "C:/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain file")
endif()

find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(sol2 CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Lua
#
# Windows resolves Lua through the vcpkg install tree; Unix resolves it from a
# system or Homebrew prefix. Both branches converge on the same Lua::Lua
# imported target so the link step below stays platform-agnostic.
#
# Pin to Lua 5.4 on both platforms: policy rules are Lua source shipped as data,
# so the language version is part of the policy contract. A 5.4 agent and a 5.5
# agent would not evaluate the same policy file identically.
# ---------------------------------------------------------------------------
if(WIN32)
  set(_vcpkg_root "C:/vcpkg" CACHE PATH "vcpkg root (fallback)")
  if(DEFINED CMAKE_TOOLCHAIN_FILE AND EXISTS "${CMAKE_TOOLCHAIN_FILE}")
    # Toolchain lives at <vcpkg_root>/scripts/buildsystems/vcpkg.cmake, so walk
    # up three levels to recover the root.
    get_filename_component(_vcpkg_from_toolchain "${CMAKE_TOOLCHAIN_FILE}" DIRECTORY)  # -> scripts/buildsystems
    get_filename_component(_vcpkg_from_toolchain "${_vcpkg_from_toolchain}" DIRECTORY) # -> scripts
    get_filename_component(_vcpkg_from_toolchain "${_vcpkg_from_toolchain}" DIRECTORY) # -> vcpkg_root
    if(EXISTS "${_vcpkg_from_toolchain}/installed/x64-windows/include")
      set(_vcpkg_root "${_vcpkg_from_toolchain}")
    endif()
  endif()
  set(_lua_hints "${_vcpkg_root}/installed/x64-windows")
else()
  # SENTINEL_LUA_ROOT lets a preset or CI job point at a specific prefix.
  # The Homebrew hints cover lua@5.4, which is keg-only and therefore absent
  # from the default CMake search path.
  set(SENTINEL_LUA_ROOT "" CACHE PATH "Lua 5.4 install prefix (optional hint)")
  set(_lua_hints
    "${SENTINEL_LUA_ROOT}"
    "/opt/homebrew/opt/lua@5.4"   # Homebrew, Apple Silicon
    "/usr/local/opt/lua@5.4"      # Homebrew, Intel
    "/usr/local"
    "/usr"
  )
endif()

find_library(LUA_LIBRARY
  NAMES lua5.4 lua-5.4 lua54 lua
  HINTS ${_lua_hints}
  PATH_SUFFIXES lib
)

# Resolve the include directory the way sol2 resolves it, not the way a normal
# Find module would.
#
# sol2 probes `__has_include(<lua/lua.h>)` first and only falls back to
# <lua.h>. On a Homebrew machine, `brew install lua` (5.5) puts a complete
# header set at /opt/homebrew/include/lua/, and /opt/homebrew/include is always
# on the include path because spdlog, sol2 and nlohmann live there. So handing
# CMake the directory that contains a bare lua.h is not enough: sol2 would still
# pick up 5.5 headers from the lua/ subdirectory of another prefix while we link
# the 5.4 library. That mismatch does not fail the build. It aborts at runtime
# with "PANIC: unprotected error in call to Lua API" the first time a policy is
# evaluated, which is easy to mistake for a policy bug.
#
# So prefer a prefix that owns lua/lua.h, and prepend it to the include path
# (see target_include_directories(... BEFORE ...) below) so it cannot be
# shadowed. LUA_SUBDIR_INCLUDE_ROOT is the prefix containing lua/;
# LUA_INCLUDE_DIR is the fallback for layouts that only ship a bare lua.h
# (vcpkg on Windows, most Linux distributions).
find_path(LUA_SUBDIR_INCLUDE_ROOT lua/lua.h
  HINTS ${_lua_hints}
  PATH_SUFFIXES include
)
find_path(LUA_INCLUDE_DIR lua.h
  HINTS ${_lua_hints}
  PATH_SUFFIXES lua5.4 lua-5.4 lua include/lua5.4 include/lua include
)

if(LUA_SUBDIR_INCLUDE_ROOT)
  set(SENTINEL_LUA_INCLUDES "${LUA_SUBDIR_INCLUDE_ROOT}" "${LUA_INCLUDE_DIR}")
  set(_lua_version_header "${LUA_SUBDIR_INCLUDE_ROOT}/lua/lua.h")
else()
  set(SENTINEL_LUA_INCLUDES "${LUA_INCLUDE_DIR}")
  set(_lua_version_header "${LUA_INCLUDE_DIR}/lua.h")
endif()
list(REMOVE_DUPLICATES SENTINEL_LUA_INCLUDES)

if(NOT (LUA_INCLUDE_DIR AND LUA_LIBRARY))
  message(FATAL_ERROR
    "Lua 5.4 not found.\n"
    "  Windows: install via vcpkg, or set -D_vcpkg_root=<path>.\n"
    "  macOS:   brew install lua@5.4\n"
    "  Linux:   apt install liblua5.4-dev\n"
    "Override detection with -DSENTINEL_LUA_ROOT=<prefix>.")
endif()

# Check the version of the header sol2 will actually see.
if(EXISTS "${_lua_version_header}")
  file(STRINGS "${_lua_version_header}" _lua_version_line REGEX "^#define[ \t]+LUA_VERSION_NUM[ \t]+")
  string(REGEX MATCH "[0-9]+" _lua_version_num "${_lua_version_line}")
  if(_lua_version_num AND NOT _lua_version_num EQUAL 504)
    message(WARNING
      "Lua headers at ${_lua_version_header} report LUA_VERSION_NUM="
      "${_lua_version_num}, but this project targets 5.4 (504). Policy "
      "evaluation semantics may differ from the Windows build.")
  endif()
endif()

if(NOT TARGET Lua::Lua)
  add_library(Lua::Lua UNKNOWN IMPORTED)
  set_target_properties(Lua::Lua PROPERTIES
    IMPORTED_LOCATION "${LUA_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${SENTINEL_LUA_INCLUDES}"
  )
endif()

message(STATUS "Lua includes: ${SENTINEL_LUA_INCLUDES}")
message(STATUS "Lua library:  ${LUA_LIBRARY}")

# ---------------------------------------------------------------------------
# Verify the Lua headers and the Lua library actually agree.
#
# Guards the failure described above: a header/library version mismatch is
# invisible at compile time and only shows up as a runtime panic. This probe
# links the real library against the real headers and asks the library what
# version it is, so the mismatch becomes a configure error instead.
# ---------------------------------------------------------------------------
if(NOT DEFINED SENTINEL_LUA_ABI_OK AND NOT CMAKE_CROSSCOMPILING)
  set(_probe_src "${CMAKE_CURRENT_BINARY_DIR}/lua_abi_probe.cpp")
  file(WRITE "${_probe_src}" "
#include <cstdio>
#include <cstring>
extern \"C\" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
int main() {
    lua_State* L = luaL_newstate();
    if (!L) { printf(\"newstate failed\\n\"); return 2; }
    luaL_openlibs(L);
    lua_getglobal(L, \"_VERSION\");
    const char* v = lua_tostring(L, -1);
    if (!v) { lua_close(L); printf(\"no _VERSION\\n\"); return 3; }
    /* Compare the header's compile-time version against the library's own. */
    char expected[32];
    snprintf(expected, sizeof(expected), \"Lua %d.%d\",
             LUA_VERSION_NUM / 100, LUA_VERSION_NUM % 100);
    int ok = (strcmp(expected, v) == 0);
    printf(\"header=%d library=%s\\n\", LUA_VERSION_NUM, v);
    lua_close(L);
    return ok ? 0 : 1;
}
")
  try_run(_lua_probe_run _lua_probe_compile
    SOURCES "${_probe_src}"
    LINK_LIBRARIES Lua::Lua
    COMPILE_OUTPUT_VARIABLE _lua_probe_build_log
    RUN_OUTPUT_VARIABLE _lua_probe_output
  )
  string(STRIP "${_lua_probe_output}" _lua_probe_output)

  if(NOT _lua_probe_compile)
    message(FATAL_ERROR
      "Could not compile a program against the detected Lua headers.\n"
      "Includes: ${SENTINEL_LUA_INCLUDES}\n"
      "Library:  ${LUA_LIBRARY}\n\n${_lua_probe_build_log}")
  elseif(NOT _lua_probe_run EQUAL 0)
    message(FATAL_ERROR
      "Lua header/library mismatch: ${_lua_probe_output}\n\n"
      "The headers and the linked library are different Lua versions. sol2 "
      "resolves <lua/lua.h> before <lua.h>, so another prefix on the include "
      "path (commonly Homebrew's 'lua' 5.5 at /opt/homebrew/include/lua) can "
      "shadow the version you meant to build against. Left unfixed this builds "
      "cleanly and then panics at runtime on first policy evaluation.\n\n"
      "Includes: ${SENTINEL_LUA_INCLUDES}\n"
      "Library:  ${LUA_LIBRARY}\n\n"
      "Point -DSENTINEL_LUA_ROOT=<prefix> at a prefix whose lua/ subdirectory "
      "matches the library above.")
  else()
    message(STATUS "Lua ABI check: ${_lua_probe_output}")
    set(SENTINEL_LUA_ABI_OK TRUE CACHE INTERNAL "Lua header/library versions verified")
  endif()
endif()

# ---------------------------------------------------------------------------
# SQLite
#
# vcpkg exports SQLite as unofficial-sqlite3; everywhere else CMake's bundled
# FindSQLite3 module locates the system or Homebrew copy.
# ---------------------------------------------------------------------------
if(WIN32)
  find_package(unofficial-sqlite3 CONFIG REQUIRED)
  set(SENTINEL_SQLITE_TARGET unofficial::sqlite3::sqlite3)
else()
  find_package(SQLite3 REQUIRED)
  # CMake 4.4 renamed the imported target and deprecated the old spelling, but
  # the new name does not exist on the 3.20 baseline this project supports.
  if(TARGET SQLite3::SQLite3)
    set(SENTINEL_SQLITE_TARGET SQLite3::SQLite3)
  else()
    set(SENTINEL_SQLITE_TARGET SQLite::SQLite3)
  endif()
  message(STATUS "SQLite3: ${SQLite3_LIBRARIES} (${SQLite3_VERSION})")
endif()

add_executable(Sentinel
  src/main.cpp
  src/osquery_runner.cpp
  src/lua_evaluator.cpp
  src/scoring.cpp
  src/db.cpp
  src/report_hasher.cpp
  src/delivery_client.cpp
  src/http_delivery_client.cpp
  src/retry_queue.cpp
)

target_include_directories(Sentinel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# BEFORE is load-bearing, not stylistic: it forces the Lua prefix ahead of the
# include directories contributed by spdlog/sol2/nlohmann (all of which are
# /opt/homebrew/include on a Homebrew machine). Without it, sol2's
# <lua/lua.h> probe can resolve against a different Lua version than the one
# we link. See the ABI probe above.
target_include_directories(Sentinel BEFORE PRIVATE ${SENTINEL_LUA_INCLUDES})

target_link_libraries(Sentinel PRIVATE
  nlohmann_json::nlohmann_json
  spdlog::spdlog
  sol2::sol2
  httplib::httplib
  Lua::Lua
  ${SENTINEL_SQLITE_TARGET}
)

# Delivery foundation integration test.
# Deliberately links a narrower set than the agent: these translation units
# pull in neither spdlog nor Lua.
add_executable(test_delivery_foundation
  tests/test_delivery_foundation.cpp
  src/db.cpp
  src/report_hasher.cpp
  src/delivery_client.cpp
  src/http_delivery_client.cpp
  src/retry_queue.cpp
)

target_include_directories(test_delivery_foundation PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(test_delivery_foundation PRIVATE
  nlohmann_json::nlohmann_json
  httplib::httplib
  ${SENTINEL_SQLITE_TARGET}
)

# Lua evaluator test. Exercises the sandboxed rule engine directly with
# synthetic osquery rows, so it needs no osquery installation and runs on any
# platform: the policy file is data, and its Lua snippets evaluate identically
# wherever the agent is built.
add_executable(test_lua_evaluator
  tests/test_lua_evaluator.cpp
  src/lua_evaluator.cpp
)

target_include_directories(test_lua_evaluator PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
# Same include-order requirement as the agent; see the note above.
target_include_directories(test_lua_evaluator BEFORE PRIVATE ${SENTINEL_LUA_INCLUDES})

target_link_libraries(test_lua_evaluator PRIVATE
  nlohmann_json::nlohmann_json
  spdlog::spdlog
  sol2::sol2
  Lua::Lua
)

# Canonicalization emitter for the cross-language differential test. Prints the
# canonical form and hash of each shared fixture; tests/canonicalization/
# compare.py diffs it against the Python and Go emitters. Not a test on its own
# — the comparison is what asserts.
add_executable(canon_dump
  tests/canonicalization/canon_dump.cpp
  src/report_hasher.cpp
)

target_include_directories(canon_dump PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(canon_dump PRIVATE nlohmann_json::nlohmann_json)

enable_testing()
add_test(NAME delivery_foundation COMMAND test_delivery_foundation)
add_test(NAME lua_evaluator
  COMMAND test_lua_evaluator "${CMAKE_CURRENT_SOURCE_DIR}/policies/macos_policy.json"
)
