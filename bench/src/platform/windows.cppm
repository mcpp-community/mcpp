// bench.platform:windows — process launch, wall-clock timing and host facts on
// Windows.
//
// SHAPE: the ENTIRE body is inside `#if defined(_WIN32)`. On POSIX this file
// still compiles and exports nothing; the peer partition exports the same names.
// Exactly one definition of each exists in any build, so the platform is chosen
// at compile time with no stubs and no dispatch. Same convention as
// xlings' src/platform/windows.cppm.
module;

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

export module bench.platform:windows;

import std;

#if defined(_WIN32)

namespace bench::platform_impl {

export constexpr std::string_view OS_NAME = "windows";

// CreateProcess takes ONE command line, not a vector, and parses it with the
// CRT rules: quote an argument containing space/tab/quote, backslash-escape
// embedded quotes, and DOUBLE any run of backslashes that immediately precedes
// a quote. That last rule is the one usually missed — it is why "works until a
// path ends in a backslash" is a classic Windows bug, and here it would change
// WHICH tree gets built rather than fail loudly.
static void append_quoted(std::string& out, const std::string& arg) {
    const bool needs = arg.empty()
        || arg.find_first_of(" \t\"") != std::string::npos;
    if (!needs) { out += arg; return; }

    out += '"';
    for (std::size_t i = 0; i < arg.size(); ) {
        std::size_t slashes = 0;
        while (i < arg.size() && arg[i] == '\\') { ++slashes; ++i; }
        if (i == arg.size()) {
            out.append(slashes * 2, '\\');
            break;
        }
        if (arg[i] == '"') {
            out.append(slashes * 2 + 1, '\\');
            out += '"';
        } else {
            out.append(slashes, '\\');
            out += arg[i];
        }
        ++i;
    }
    out += '"';
}

export int run_process(const std::vector<std::string>& argv,
                       const std::filesystem::path&    cwd,
                       const std::filesystem::path&    log,
                       double*                         out_wall_s) {
    if (out_wall_s) *out_wall_s = 0.0;
    if (argv.empty()) return -1;

    std::string cmdline;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i) cmdline += ' ';
        append_quoted(cmdline, argv[i]);
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    const std::string log_s = log.empty() ? std::string("NUL") : log.string();
    HANDLE sink = ::CreateFileA(log_s.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                                log.empty() ? OPEN_EXISTING : CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    if (sink != INVALID_HANDLE_VALUE) {
        si.dwFlags    = STARTF_USESTDHANDLES;
        si.hStdOutput = sink;
        si.hStdError  = sink;
        si.hStdInput  = ::GetStdHandle(STD_INPUT_HANDLE);
    }

    LARGE_INTEGER freq{}, t0{}, t1{};
    ::QueryPerformanceFrequency(&freq);
    ::QueryPerformanceCounter(&t0);

    const std::string  cwd_s = cwd.string();
    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr,
                                     /*bInheritHandles*/ TRUE, 0, nullptr,
                                     cwd.empty() ? nullptr : cwd_s.c_str(), &si, &pi);
    if (!ok) {
        if (sink != INVALID_HANDLE_VALUE) ::CloseHandle(sink);
        return -1;
    }

    ::WaitForSingleObject(pi.hProcess, INFINITE);
    ::QueryPerformanceCounter(&t1);

    DWORD code = 0;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    if (sink != INVALID_HANDLE_VALUE) ::CloseHandle(sink);

    if (out_wall_s && freq.QuadPart > 0)
        *out_wall_s = static_cast<double>(t1.QuadPart - t0.QuadPart)
                    / static_cast<double>(freq.QuadPart);
    return static_cast<int>(code);
}

// Peer of the POSIX setenv/unsetenv. `SetEnvironmentVariableA(key, nullptr)`
// is the documented way to REMOVE a variable — passing "" would leave an empty
// one behind, which a child process sees as set.
export void set_env(const std::string& key, const std::string& value) {
    ::SetEnvironmentVariableA(key.c_str(), value.c_str());
}

export void unset_env(const std::string& key) {
    ::SetEnvironmentVariableA(key.c_str(), nullptr);
}

export int cpu_logical() {
    SYSTEM_INFO si{};
    ::GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? static_cast<int>(si.dwNumberOfProcessors) : 1;
}

// The relationship table needs the two-call pattern: its length is not knowable
// up front, so ask for the size, allocate, then ask again.
static std::vector<unsigned char> processor_info(LOGICAL_PROCESSOR_RELATIONSHIP rel) {
    DWORD bytes = 0;
    ::GetLogicalProcessorInformationEx(rel, nullptr, &bytes);
    if (bytes == 0) return {};
    std::vector<unsigned char> buf(bytes);
    if (!::GetLogicalProcessorInformationEx(
            rel, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()),
            &bytes))
        return {};
    buf.resize(bytes);
    return buf;
}

export int cpu_physical() {
    auto buf = processor_info(RelationProcessorCore);
    int  count = 0;
    for (DWORD off = 0; off < buf.size(); ) {
        auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + off);
        if (info->Size == 0) break;
        ++count;
        off += info->Size;
    }
    return count > 0 ? count : cpu_logical();
}

export std::string cpu_model() {
    HKEY key{};
    if (::RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                        "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                        0, KEY_READ, &key) != ERROR_SUCCESS)
        return {};
    char  buf[256] = {};
    DWORD size = sizeof(buf);
    DWORD type = 0;
    const LSTATUS st = ::RegQueryValueExA(key, "ProcessorNameString", nullptr, &type,
                                          reinterpret_cast<LPBYTE>(buf), &size);
    ::RegCloseKey(key);
    if (st != ERROR_SUCCESS || type != REG_SZ) return {};
    return std::string(buf);
}

export std::uint64_t ram_bytes() {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (::GlobalMemoryStatusEx(&status)) return status.ullTotalPhys;
    return 0;
}

// Windows states efficiency class per core; more than one distinct class is
// exactly what "hybrid" means here — no frequency heuristics needed.
export bool heterogeneous_cpu() {
    auto buf = processor_info(RelationProcessorCore);
    int  first = -1;
    for (DWORD off = 0; off < buf.size(); ) {
        auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + off);
        if (info->Size == 0) break;
        const int cls = static_cast<int>(info->Processor.EfficiencyClass);
        if (first < 0) first = cls;
        else if (cls != first) return true;
        off += info->Size;
    }
    return false;
}

}  // namespace bench::platform_impl

#endif  // _WIN32
