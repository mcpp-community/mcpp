// bench.platform — implementation.
//
// `module bench.platform;` with no `export`: an implementation unit. The
// interface declares; the bodies live here, so nothing below reaches an
// importer's BMI. Everything the suite does to the OS still passes through this
// one module, and the `#if defined(...)` blocks the partitions cannot own — the
// architecture probe, and MSVC's getenv deprecation — stay confined to this
// file rather than spreading into runner or the engines.
//
// A module implementation unit implicitly imports its primary interface, so the
// `platform_impl::` names the :posix / :windows partitions export are visible.
module bench.platform;

import std;

namespace bench::platform {

ScopedEnv::ScopedEnv(std::string key, const std::string& value) : key_(std::move(key)) {
        // MSVC's CRT deprecates getenv in favour of _dupenv_s. Reading it is
        // safe here (single-threaded setup, value copied immediately) and the
        // portable spelling keeps this out of the platform partitions.
#if defined(_MSC_VER)
#pragma warning(suppress : 4996)
#endif
        if (const char* prev = std::getenv(key_.c_str())) {
            had_previous_ = true;
            previous_     = prev;
        }
        set_env(key_, value);
    }
    ScopedEnv::~ScopedEnv() {
        if (had_previous_) set_env(key_, previous_);
        else               unset_env(key_);
    }

bool RunResult::ok() const { return exit_code == 0; }

bool RunResult::started() const { return !launch_failed; }

RunResult run(const std::vector<std::string>& argv,
                     const std::filesystem::path&    cwd,
                     const std::filesystem::path&    log,
                     double                          timeout_s) {
    double wall = 0.0;
    bool   hung = false;
    std::string why;
    bool failed_to_launch = false;
    const int rc = run_process(argv, cwd, log, &wall, timeout_s, &hung, &why,
                               &failed_to_launch);
    return RunResult{wall, rc, hung, std::move(why), failed_to_launch};
}

bool log_mentions(const std::filesystem::path& p,
                         std::initializer_list<std::string_view> markers) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line))
        for (const auto m : markers)
            if (line.find(m) != std::string::npos) return true;
    return false;
}

std::string log_grep(const std::filesystem::path& p,
                            std::initializer_list<std::string_view> markers,
                            std::size_t max) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::string out, line;
    std::size_t kept = 0;
    // THE MESSAGE IS OFTEN ON THE NEXT LINE. cmake writes
    //     CMake Error at xpkg_source_library.cmake:231 (message):
    //       bench: <pkg>'s manifest has no `sources` list
    // and a grep that returns only matching lines keeps the location and throws
    // away WHAT WENT WRONG. That happened three separate times today, each time
    // costing a round trip to CI for a sentence that was already in the file.
    // `after` carries the following lines of a hit through.
    std::size_t after = 0;
    while (kept < max && std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        bool hit = false;
        for (const auto m : markers)
            if (line.find(m) != std::string::npos) { hit = true; break; }
        if (!hit) {
            if (after == 0 || line.empty()) continue;
            --after;                       // trailing context of the previous hit
        } else {
            after = 2;
        }
        // Long lines here are usually a whole compiler command line; the cause
        // is at the front of them.
        if (line.size() > 400) { line.resize(400); line += " …"; }
        out += "    "; out += line; out += '\n';
        ++kept;
    }
    return out;
}

std::string tail_of(const std::filesystem::path& p, std::size_t lines) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::deque<std::string> keep;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        keep.push_back(std::move(line));
        if (keep.size() > lines) keep.pop_front();
    }
    std::string out;
    for (const auto& l : keep) { out += l; out += '\n'; }
    return out;
}

bool have_program(const std::vector<std::string>& version_argv) {
    return run(version_argv).started();
}

std::optional<std::string> run_capture(const std::vector<std::string>& argv,
                                              const std::filesystem::path& cwd,
                                              RunResult* result) {
    std::error_code ec;
    auto tmp = std::filesystem::temp_directory_path(ec);
    if (ec) return std::nullopt;
    tmp /= std::format("bench-capture-{}.txt",
                       std::chrono::steady_clock::now().time_since_epoch().count());

    const auto r = run(argv, cwd, tmp);
    if (result) *result = r;
    if (!r.started()) { std::filesystem::remove(tmp, ec); return std::nullopt; }

    std::ifstream in(tmp, std::ios::binary);
    std::string text;
    if (in) text.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    std::filesystem::remove(tmp, ec);
    return text;
}

HostFacts host_facts() {
    HostFacts f;
    f.os             = std::string(OS_NAME);
    f.cpu_model      = platform_impl::cpu_model();
    f.logical_cores  = platform_impl::cpu_logical();
    f.physical_cores = platform_impl::cpu_physical();
    f.heterogeneous  = platform_impl::heterogeneous_cpu();
    f.ram_bytes      = platform_impl::ram_bytes();
    // Architecture, unlike the OS, is not a partition concern: both partitions
    // would carry an identical copy of this. It is a property of the build, so
    // it is detected once, here.
#if defined(__aarch64__) || defined(_M_ARM64)
    f.arch = "aarch64";
#elif defined(__x86_64__) || defined(_M_X64)
    f.arch = "x86_64";
#else
    f.arch = "unknown";
#endif
    return f;
}

void remove_tree(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::remove_all(p, ec);   // absent is success, not failure
}

bool touch(const std::filesystem::path& p) {
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) return false;
    std::filesystem::last_write_time(p, std::filesystem::file_time_type::clock::now(), ec);
    return !ec;
}

std::string iso_now() {
    return std::format("{:%FT%TZ}",
                       std::chrono::floor<std::chrono::seconds>(
                           std::chrono::system_clock::now()));
}

}  // namespace bench::platform
