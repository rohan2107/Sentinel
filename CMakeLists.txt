cmake_minimum_required(VERSION 3.20)
project(SentinelAgent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# vcpkg toolchain
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND EXISTS "C:/vcpkg/scripts/buildsystems/vcpkg.cmake")
  set(CMAKE_TOOLCHAIN_FILE "C:/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain file")
endif()

find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(sol2 CONFIG REQUIRED)

# Manual Lua detection for Windows/vcpkg
set(_vcpkg_root "C:/vcpkg" CACHE PATH "vcpkg root (fallback)")
find_path(LUA_INCLUDE_DIR lua.h HINTS "${_vcpkg_root}/installed/x64-windows/include" PATH_SUFFIXES lua include)
find_library(LUA_LIBRARY NAMES lua54 lua5.4 lua HINTS "${_vcpkg_root}/installed/x64-windows/lib")
if (LUA_INCLUDE_DIR AND LUA_LIBRARY)
  if (NOT TARGET Lua::Lua)
    add_library(Lua::Lua UNKNOWN IMPORTED)
    set_target_properties(Lua::Lua PROPERTIES
      IMPORTED_LOCATION "${LUA_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${LUA_INCLUDE_DIR}"
    )
  endif()
else()
  message(FATAL_ERROR "Lua not found. Install lua via vcpkg or adjust _vcpkg_root.")
endif()

# SQLite (vcpkg exposes unofficial-sqlite3)
find_package(unofficial-sqlite3 CONFIG REQUIRED)

add_executable(SentinelAgent
  src/main.cpp
  src/osquery_runner.cpp
  src/lua_evaluator.cpp
  src/scoring.cpp
  src/db.cpp
)

target_include_directories(SentinelAgent PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(SentinelAgent PRIVATE
  nlohmann_json::nlohmann_json
  spdlog::spdlog
  sol2::sol2
  Lua::Lua
  unofficial::sqlite3::sqlite3
)
