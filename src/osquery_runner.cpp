// src/osquery_runner.cpp
#include "osquery_runner.h"
#include <stdexcept>
#include <sstream>
#include <vector>
#include <string>
#include <cstdio>
#include <chrono>
#include <system_error>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#  include <Windows.h>
#else
#  include <spawn.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  include <signal.h>
extern char **environ;
#endif

#ifdef _WIN32
#  define POPEN _popen
#  define PCLOSE _pclose
#else
#  define POPEN popen
#  define PCLOSE pclose
#endif

using json = nlohmann::json;

namespace {

const std::chrono::milliseconds kOsqueryTimeout{10000}; // hard timeout to avoid hangs
constexpr size_t kMaxOutputBytes = 1 * 1024 * 1024;      // guardrail for huge outputs

#ifdef _WIN32
// Minimal Windows command-line argument escaping (compatible with CommandLineToArgvW rules)
static std::string win_escape_arg(const std::string& arg) {
    bool needs_quotes = arg.find_first_of(" \t\"") != std::string::npos;
    if (!needs_quotes) return arg;
    std::string out = "\"";
    int backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            backslashes++;
        } else if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
            backslashes = 0;
        } else {
            if (backslashes) out.append(backslashes, '\\');
            backslashes = 0;
            out.push_back(c);
        }
    }
    if (backslashes) out.append(backslashes * 2, '\\');
    out.push_back('"');
    return out;
}
#endif

} // namespace

json run_osquery_json(const std::string& sql_query) {
#ifdef _WIN32
    // Windows: use CreateProcess with redirected stdout (no shell), apply timeout.
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        throw std::runtime_error("CreatePipe failed");
    }
    if (!SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(readPipe); CloseHandle(writePipe);
        throw std::runtime_error("SetHandleInformation failed");
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe; // capture stderr too

    PROCESS_INFORMATION pi{};

    std::vector<std::string> args = {
        "osqueryi",
        "--json",
        "--disable_events",
        "--disable_logging",
        "--disable_watchdog",
        "--timeout", "10",
        sql_query
    };
    std::ostringstream cmdline;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) cmdline << ' ';
        cmdline << win_escape_arg(args[i]);
    }
    std::string cmd = cmdline.str();

    BOOL ok = CreateProcessA(
        nullptr,
        cmd.data(),
        nullptr,
        nullptr,
        TRUE, // inherit handles for stdout/stderr
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi);

    CloseHandle(writePipe); // parent no longer writes

    if (!ok) {
        CloseHandle(readPipe);
        throw std::runtime_error("Failed to launch osqueryi (CreateProcess)");
    }

    std::string result;
    result.reserve(4096);
    DWORD wait = WaitForSingleObject(pi.hProcess, static_cast<DWORD>(kOsqueryTimeout.count()));
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
    }

    // Drain stdout
    constexpr DWORD chunk = 16 * 1024;
    char buffer[chunk];
    DWORD read = 0;
    while (ReadFile(readPipe, buffer, chunk, &read, nullptr) && read > 0) {
        if (result.size() + read > kMaxOutputBytes) {
            CloseHandle(readPipe);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            throw std::runtime_error("osquery output exceeded limit");
        }
        result.append(buffer, buffer + read);
    }
    CloseHandle(readPipe);

    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

#else
    // POSIX: spawn without shell using posix_spawnp, capture stdout via pipe, enforce timeout.
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        throw std::system_error(errno, std::generic_category(), "pipe failed");
    }

    pid_t pid = 0;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);

    std::vector<char*> argv;
    std::string query_copy = sql_query;
    argv.push_back(const_cast<char*>("osqueryi"));
    argv.push_back(const_cast<char*>("--json"));
    argv.push_back(const_cast<char*>("--disable_events"));
    argv.push_back(const_cast<char*>("--disable_logging"));
    argv.push_back(const_cast<char*>("--disable_watchdog"));
    argv.push_back(const_cast<char*>("--timeout"));
    argv.push_back(const_cast<char*>("10"));
    argv.push_back(query_copy.data());
    argv.push_back(nullptr);

    int spawn_rc = posix_spawnp(&pid, "osqueryi", &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]); // parent write end

    if (spawn_rc != 0) {
        close(pipefd[0]);
        throw std::system_error(spawn_rc, std::generic_category(), "posix_spawnp failed");
    }

    std::string result;
    result.reserve(4096);
    constexpr size_t chunk = 16 * 1024;
    char buffer[chunk];
    auto start = std::chrono::steady_clock::now();
    ssize_t n = 0;
    while ((n = read(pipefd[0], buffer, chunk)) > 0) {
        if (result.size() + static_cast<size_t>(n) > kMaxOutputBytes) {
            close(pipefd[0]);
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
            throw std::runtime_error("osquery output exceeded limit");
        }
        result.append(buffer, buffer + n);
        if (std::chrono::steady_clock::now() - start > kOsqueryTimeout) {
            close(pipefd[0]);
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
            throw std::runtime_error("osquery timed out");
        }
    }
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif

    try {
        if (result.empty()) {
            return json::array();
        }
        json parsed = json::parse(result);
        if (exit_code != 0) {
            // Non-zero with JSON output: surface as error to caller for logging/handling.
            std::ostringstream oss;
            oss << "osquery exited with code " << exit_code << " but returned JSON";
            throw std::runtime_error(oss.str());
        }
        return parsed;
    } catch (const std::exception& e) {
        std::ostringstream oss;
        oss << "osqueryi returned invalid JSON (exit code " << exit_code << "): " << e.what()
            << "\nraw output:\n" << result;
        throw std::runtime_error(oss.str());
    }
}
