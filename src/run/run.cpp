// mcpp.run over mcpplibs.os: the launcher, the registry of running programs,
// and the watcher that ends mcpp at a request.
module;
#include <cstdio>     // stdout, stderr: macros, which `import std` does not carry

module mcpp.run;

import std;
import mcpplibs.os.platform;
import mcpplibs.os.error;
import mcpplibs.os.env;
import mcpplibs.os.fs;
import mcpplibs.os.process;
import mcpplibs.os.interrupt;
import mcpp.invocation;
import mcpp.build.command_line;
import mcpp.runtime.launch_env;

namespace mcpp::run {

namespace {

namespace os = mcpplibs::os;
using Process = os::process::process;

// ─── What the watcher ends, writes and runs ──────────────────────────────

std::mutex g_live;
std::set<Process*>& live() {
    static std::set<Process*> s;
    return s;
}

std::mutex g_files;
std::vector<std::pair<std::filesystem::path, std::string>>& interrupt_files() {
    static std::vector<std::pair<std::filesystem::path, std::string>> f;
    return f;
}

std::mutex g_cleanups;
int g_next_cleanup = 0;
std::map<int, std::function<void()>>& cleanups() {
    static std::map<int, std::function<void()>> c;
    return c;
}

// A started program, ended with mcpp at a request for as long as this lives.
struct Registered {
    Process* p;
    explicit Registered(Process* q) : p(q) { std::lock_guard g(g_live); live().insert(p); }
    ~Registered() { std::lock_guard g(g_live); live().erase(p); }
    Registered(const Registered&) = delete;
    Registered& operator=(const Registered&) = delete;
};

// Everything mcpp owes before it ends at a request or a closed output, once.
std::atomic<bool> g_finishing{false};
void finish() {
    if (g_finishing.exchange(true)) return;
    // The files first: what the request leaves is the state the build was in
    // when it arrived. A program ended below wakes the thread that waited on
    // it, and what that thread then records is not written (`ending`).
    {
        std::lock_guard g(g_files);
        for (auto const& [path, content] : interrupt_files())
            (void)os::fs::write_atomic(path, content);
    }
    {
        std::lock_guard g(g_live);
        for (auto* p : live()) { p->end_unit(); p->kill(); }
    }
    std::vector<std::function<void()>> todo;
    {
        std::lock_guard g(g_cleanups);
        for (auto& [k, f] : cleanups()) todo.push_back(f);
    }
    for (auto& f : todo) f();
}

// ─── The launcher ────────────────────────────────────────────────────────

enum class Output { inherit, merged, stdout_only, stdout_err_shown, stdout_err_kept, discard };

struct Spec {
    std::vector<std::string> argv;
    Env env;
    std::vector<std::string> unset;
    bool hostTool = false;
    std::string cwd;
    Output out = Output::inherit;
    bool nullInput = false;
    std::chrono::milliseconds total{0}, idle{0};
    const std::function<void(std::string_view)>* onLine = nullptr;
    bool splitOnCr = false;
    const std::function<bool()>* shouldStop = nullptr;
    bool oneString = false;
};

struct Outcome {
    int exit_code = 127;
    std::string output;
    std::string error_output;
    bool timed_out = false;
    int spawn_error = 0;            // errno-style; 0 when started
    std::string spawn_message;
};

// The errno a caller classifies a refused start by (ENOENT, EACCES, ENOEXEC).
int errno_of(os::errc c) {
    switch (c) {
        case os::errc::not_found:   return 2;
        case os::errc::permission:  return 13;
        case os::errc::not_program: return 8;
        default:                    return 5;
    }
}

std::string spawn_failure(std::string_view program, int error) {
    return std::format("cannot start '{}' (error {}): {}\n", program, error,
                       std::generic_category().message(error));
}

// The environment a started program receives: this invocation's statements,
// the inherited loader search paths without mcpp's private glibc (an outer
// `mcpp run` points them at a payload a host tool must not load), and the
// caller's additions, which win.
std::vector<std::pair<std::string, std::optional<std::string>>> child_env(const Env& extra, bool hostTool,
                                                                         const std::vector<std::string>& unset = {}) {
    auto env = mcpp::invocation::overrides();
    auto stated = [&](std::string_view key) {
        for (auto& [k, v] : env) if (k == key) return true;
        for (auto& [k, v] : extra) if (k == key) return true;
        return false;
    };
    for (std::string_view key : { std::string_view("LD_LIBRARY_PATH"), std::string_view("DYLD_LIBRARY_PATH") }) {
        if (stated(key)) continue;
        if (auto v = os::env::get(key)) {
            auto cleaned = mcpp::runtime::launch_env::strip_private_glibc(*v);
            env.emplace_back(std::string(key), cleaned.empty() ? std::nullopt
                                                               : std::optional<std::string>(cleaned));
        }
    }
    if (hostTool)
        if (auto key = mcpp::runtime::launch_env::host_tool_runtime_library_path_key(); !key.empty())
            env.emplace_back(std::move(key), std::nullopt);
    for (auto& [k, v] : extra) env.emplace_back(k, v);
    for (auto& k : unset) env.emplace_back(k, std::nullopt);
    return env;
}

// A bare name looked for on this invocation's PATH, which mcpp may have
// stated; a name with a separator is taken as written.
std::optional<std::filesystem::path> program_of(const std::string& name) {
    if (os::fs::names_a_place(name)) return std::filesystem::path(name);
    return mcpp::invocation::which(name);
}

// Lines of `acc` up to its last line end, to `on_line`, without the ends.
void deliver_lines(std::string& acc, const std::function<void(std::string_view)>& on_line, bool splitOnCr,
                   bool all) {
    std::size_t start = 0;
    for (std::size_t i = 0; i < acc.size(); ++i) {
        const bool lf = acc[i] == '\n';
        const bool cr = splitOnCr && acc[i] == '\r' && i + 1 < acc.size() && acc[i + 1] != '\n';
        if (!lf && !cr) continue;
        std::string_view line(acc.data() + start, i - start);
        while (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        on_line(line);
        start = i + 1;
    }
    acc.erase(0, start);
    if (all && !acc.empty()) {
        std::string_view line(acc);
        while (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!line.empty()) on_line(line);
        acc.clear();
    }
}

Outcome launch(const Spec& s) {
    Outcome out;
    if (s.argv.empty()) return out;
    const auto program = program_of(s.argv.front());
    if (!program) {
        out.spawn_error = 2;
        out.spawn_message = spawn_failure(s.argv.front(), 2);
        return out;
    }
    os::process::spawn_options o;
    o.program = *program;
    o.argv0 = s.argv.front();
    o.args.assign(s.argv.begin() + 1, s.argv.end());
    o.cwd = s.cwd;
    o.env = child_env(s.env, s.hostTool, s.unset);
    o.in = s.nullInput ? os::process::stdio::null : os::process::stdio::inherit;
    switch (s.out) {
        case Output::inherit:     o.out = os::process::stdio::inherit; o.err = os::process::stdio::inherit; break;
        case Output::merged:      o.out = os::process::stdio::pipe;    o.err = os::process::stdio::output;  break;
        case Output::stdout_only: o.out = os::process::stdio::pipe;    o.err = os::process::stdio::null;    break;
        case Output::stdout_err_shown: o.out = os::process::stdio::pipe; o.err = os::process::stdio::inherit; break;
        case Output::stdout_err_kept:  o.out = os::process::stdio::pipe; o.err = os::process::stdio::pipe;    break;
        case Output::discard:     o.out = os::process::stdio::null;    o.err = os::process::stdio::null;    break;
    }
    o.own_unit = true;
    o.bound_lifetime = true;
    o.one_string = s.oneString;
    auto p = Process::spawn(o);
    if (!p) {
        out.spawn_error = errno_of(p.error().code);
        out.spawn_message = spawn_failure(s.argv.front(), out.spawn_error);
        return out;
    }
    Registered guard(&*p);
    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    auto lastOutput = start;
    bool stop = false;
    const bool keepErr = s.out == Output::stdout_err_kept;
    bool errDone = !keepErr;
    // The error stream, read as it arrives so that a program writing much of
    // it is never stopped on a full pipe while its output is read.
    auto drain_error = [&](std::chrono::milliseconds wait) {
        if (errDone) return;
        auto e = p->read_error_for(wait);
        if (!e) { errDone = true; return; }
        if (!e->has_value()) return;
        if ((*e)->empty()) { errDone = true; return; }
        out.error_output += **e;
    };
    if (s.out == Output::merged || s.out == Output::stdout_only || s.out == Output::stdout_err_shown
        || keepErr) {
        std::string acc;
        std::optional<clock::time_point> endedAt;
        for (;;) {
            const auto now = clock::now();
            if ((s.total.count() > 0 && now - start >= s.total)
                || (s.idle.count() > 0 && now - lastOutput >= s.idle)
                || (s.shouldStop && *s.shouldStop && (*s.shouldStop)())) {
                stop = true;
                break;
            }
            // A program that has ended may leave one behind holding its
            // output open; what arrives in the next moment is read, then the
            // rest of its unit is ended.
            if (!endedAt) {
                if (auto w = p->try_wait(); w && w->has_value()) endedAt = now;
            } else if (now - *endedAt > std::chrono::milliseconds(500)) {
                p->end_unit();
            }
            drain_error(std::chrono::milliseconds(0));
            auto chunk = p->read_output_for(std::chrono::milliseconds(keepErr ? 20 : 100));
            if (!chunk) break;
            if (!chunk->has_value()) continue;
            if ((*chunk)->empty()) break;   // the end of the output
            lastOutput = clock::now();
            if (s.onLine) {
                acc += **chunk;
                deliver_lines(acc, *s.onLine, s.splitOnCr, false);
            } else {
                out.output += **chunk;
            }
        }
        if (s.onLine && !stop) deliver_lines(acc, *s.onLine, s.splitOnCr, true);
        // The rest of the error stream, for as long as the program and the
        // bounds allow.
        while (!stop && !errDone) {
            const auto now = clock::now();
            if (s.total.count() > 0 && now - start >= s.total) { stop = true; break; }
            if (!endedAt) {
                if (auto w = p->try_wait(); w && w->has_value()) endedAt = now;
            } else if (now - *endedAt > std::chrono::milliseconds(500)) {
                p->end_unit();
                break;
            }
            drain_error(std::chrono::milliseconds(100));
        }
    } else if (s.total.count() > 0) {
        auto w = p->wait_for(s.total);
        if (w && !w->has_value()) stop = true;
    }
    if (stop) {
        p->end_unit();
        p->kill();
        out.timed_out = true;
    }
    auto st = p->wait();
    out.exit_code = st ? st->shell_code() : 127;
    return out;
}

RunResult result_of(Outcome&& o, int* spawn_error, bool messageIntoOutput) {
    RunResult r;
    r.exit_code = o.exit_code;
    r.output = std::move(o.output);
    if (o.spawn_error != 0) {
        r.exit_code = 127;
        if (spawn_error) *spawn_error = o.spawn_error;
        else if (messageIntoOutput) r.output = o.spawn_message;
    }
    return r;
}

int status_of(const Outcome& o, int* spawn_error) {
    if (o.spawn_error == 0) return o.exit_code;
    if (spawn_error) *spawn_error = o.spawn_error;
    else std::fputs(o.spawn_message.c_str(), stderr);
    return 127;
}

const std::string& posix_shell() {
    static const std::string sh = [] {
        std::error_code ec;
        if (std::filesystem::exists("/bin/sh", ec)) return std::string("/bin/sh");
        if (std::filesystem::exists("/system/bin/sh", ec)) return std::string("/system/bin/sh");
        if (auto prefix = mcpp::invocation::get("PREFIX"))
            if (std::filesystem::exists(std::filesystem::path(*prefix) / "bin" / "sh", ec))
                return (std::filesystem::path(*prefix) / "bin" / "sh").string();
        return std::string("/bin/sh");
    }();
    return sh;
}

struct Background {
    std::optional<Process> process;
    std::optional<Registered> guard;
};

}  // namespace

// ─── This process ────────────────────────────────────────────────────────

void install() {
    (void)os::interrupt::requested();   // the requests to end are taken over
    if (!os::interrupt::available()) return;
    std::thread([] {
        while (!os::interrupt::wait()) {}
        finish();
        os::interrupt::end_as_requested();
    }).detach();
}

bool ending() { return g_finishing.load(); }

void set_interrupt_files(const std::vector<std::pair<std::filesystem::path, std::string>>& files) {
    std::lock_guard g(g_files);
    interrupt_files() = files;
}

void clear_interrupt_files() {
    std::lock_guard g(g_files);
    interrupt_files().clear();
}

int add_cleanup(std::function<void()> cleanup) {
    std::lock_guard g(g_cleanups);
    const int handle = g_next_cleanup++;
    cleanups().emplace(handle, std::move(cleanup));
    return handle;
}

void remove_cleanup(int handle) {
    std::lock_guard g(g_cleanups);
    cleanups().erase(handle);
}

void end_for_closed_output() {
    if constexpr (os::is_windows) return;   // the write is refused and mcpp goes on
    finish();
    os::interrupt::end_as_output_closed();
}

// ─── Starting a program ──────────────────────────────────────────────────

int run_exec(const std::vector<std::string>& argv, const Env& extraEnv, int* spawn_error) {
    if (spawn_error) *spawn_error = 0;
    return status_of(launch({ .argv = argv, .env = extraEnv }), spawn_error);
}

int run_foreground(const std::vector<std::string>& argv, const Env& extraEnv, int* spawn_error) {
    if (spawn_error) *spawn_error = 0;
    if (argv.empty()) return 127;
    const auto program = program_of(argv.front());
    int error = 2;
    if (program) {
        os::process::spawn_options o;
        o.program = *program;
        o.argv0 = argv.front();
        o.args.assign(argv.begin() + 1, argv.end());
        o.env = child_env(extraEnv, false);
        o.in = o.out = o.err = os::process::stdio::inherit;
        std::fflush(stdout);
        std::fflush(stderr);
        auto r = os::process::exec(o);   // returns only when the program could not be started
        error = r ? 5 : errno_of(r.error().code);
    }
    if (spawn_error) *spawn_error = error;
    else std::fputs(spawn_failure(argv.front(), error).c_str(), stderr);
    return 127;
}

RunResult capture_exec(const std::vector<std::string>& argv, const Env& extraEnv,
                       std::string_view cwd, int* spawn_error) {
    if (spawn_error) *spawn_error = 0;
    return result_of(launch({ .argv = argv, .env = extraEnv, .cwd = std::string(cwd), .out = Output::merged }),
                     spawn_error, true);
}

RunResult capture_stdout(const std::vector<std::string>& argv, const Env& extraEnv, int* spawn_error) {
    if (spawn_error) *spawn_error = 0;
    auto o = launch({ .argv = argv, .env = extraEnv, .out = Output::stdout_only, .nullInput = true });
    return result_of(std::move(o), spawn_error, false);
}

RunResult capture_host_tool_stdout(const std::vector<std::string>& argv) {
    auto o = launch({ .argv = argv, .hostTool = true, .out = Output::stdout_only, .nullInput = true });
    return result_of(std::move(o), nullptr, false);
}

RunResult capture_host_tool(const std::vector<std::string>& argv, const Env& extraEnv) {
    auto o = launch({ .argv = argv, .env = extraEnv, .hostTool = true, .out = Output::merged, .nullInput = true });
    return result_of(std::move(o), nullptr, true);
}

int run_exec_deadline(const std::vector<std::string>& argv, const Env& extraEnv,
                      std::chrono::milliseconds deadline, bool* timed_out, int* spawn_error) {
    if (spawn_error) *spawn_error = 0;
    auto o = launch({ .argv = argv, .env = extraEnv, .total = deadline });
    if (timed_out) *timed_out = o.timed_out;
    return status_of(o, spawn_error);
}

RunResult capture_exec_deadline(const std::vector<std::string>& argv, const Env& extraEnv,
                                std::chrono::milliseconds deadline, bool* timed_out,
                                std::string_view cwd, int* spawn_error) {
    if (spawn_error) *spawn_error = 0;
    auto o = launch({ .argv = argv, .env = extraEnv, .cwd = std::string(cwd), .out = Output::merged,
                      .total = deadline });
    if (timed_out) *timed_out = o.timed_out;
    return result_of(std::move(o), spawn_error, true);
}

int stream_exec(const std::vector<std::string>& argv, const Env& extraEnv,
                std::chrono::milliseconds deadline,
                const std::function<void(std::string_view line)>& on_line, bool* timed_out,
                int* spawn_error) {
    if (spawn_error) *spawn_error = 0;
    auto o = launch({ .argv = argv, .env = extraEnv, .out = Output::merged, .nullInput = true,
                      .total = deadline, .onLine = &on_line });
    if (timed_out) *timed_out = o.timed_out;
    if (o.spawn_error != 0) {
        if (spawn_error) *spawn_error = o.spawn_error;
        else on_line(std::string_view(o.spawn_message).substr(0, o.spawn_message.size() - 1));
        return 127;
    }
    return o.exit_code;
}

// ─── Any program, as the caller states ───────────────────────────────────

RunResult execute(const std::vector<std::string>& argv, const Options& opt) {
    Spec s{ .argv = argv, .env = opt.env, .unset = opt.unset, .hostTool = opt.host_tool, .cwd = opt.cwd,
            .nullInput = opt.input == Input::none, .total = opt.total, .idle = opt.idle,
            .onLine = opt.on_line ? &opt.on_line : nullptr, .splitOnCr = opt.split_on_cr,
            .shouldStop = opt.should_stop ? &opt.should_stop : nullptr, .oneString = opt.one_string };
    switch (opt.streams) {
        case Streams::inherit:            s.out = Output::inherit; break;
        case Streams::merged:             s.out = Output::merged; break;
        case Streams::output:             s.out = Output::stdout_only; break;
        case Streams::output_error_shown: s.out = Output::stdout_err_shown; break;
        case Streams::output_error_kept:  s.out = Output::stdout_err_kept; break;
        case Streams::discard:            s.out = Output::discard; break;
    }
    auto o = launch(s);
    RunResult r;
    r.exit_code = o.exit_code;
    r.output = std::move(o.output);
    r.error_output = std::move(o.error_output);
    r.timed_out = o.timed_out;
    r.spawn_error = o.spawn_error;
    if (o.spawn_error != 0) {
        r.exit_code = 127;
        if (opt.report_spawn_failure) {
            if (opt.streams == Streams::merged) {
                if (opt.on_line) opt.on_line(std::string_view(o.spawn_message).substr(0, o.spawn_message.size() - 1));
                else r.output = o.spawn_message;
            } else {
                std::fputs(o.spawn_message.c_str(), stderr);
            }
        }
    }
    return r;
}

// ─── A command for the command interpreter ───────────────────────────────

Shell shell(std::string_view command) {
    if constexpr (os::is_windows) {
        Shell sh{ { "cmd.exe" }, true };
        for (auto& a : mcpp::build::command_line::windows_shell_arguments(command)) sh.argv.push_back(std::move(a));
        return sh;
    } else {
        return { { posix_shell(), "-c", std::string(command) }, false };
    }
}

RunResult capture_shell(std::string_view command, const Env& extraEnv) {
    auto sh = shell(command);
    auto o = launch({ .argv = sh.argv, .env = extraEnv, .out = Output::stdout_err_shown, .nullInput = true,
                      .oneString = sh.one_string });
    return result_of(std::move(o), nullptr, true);
}

int run_shell_deadline(std::string_view command, std::string_view cwd,
                       std::chrono::milliseconds deadline, bool* timed_out) {
    if (timed_out) *timed_out = false;
    if (command.empty()) return 127;
    auto sh = shell(command);
    auto o = launch({ .argv = sh.argv, .cwd = std::string(cwd), .total = deadline, .oneString = sh.one_string });
    if (timed_out) *timed_out = o.timed_out;
    return o.spawn_error != 0 ? 127 : o.exit_code;
}

RunResult capture_shell_deadline(std::string_view command, const Env& extraEnv,
                                 std::chrono::milliseconds deadline, bool* timed_out,
                                 std::string_view cwd) {
    if (timed_out) *timed_out = false;
    auto sh = shell(command);
    auto o = launch({ .argv = sh.argv, .env = extraEnv, .cwd = std::string(cwd), .out = Output::merged,
                      .nullInput = true, .total = deadline, .oneString = sh.one_string });
    if (timed_out) *timed_out = o.timed_out;
    return result_of(std::move(o), nullptr, true);
}

// ─── A command mcpp owns for longer than one call ────────────────────────

BackgroundCommand start_shell_background(std::string_view command, std::string_view cwd, bool inheritStdio) {
    BackgroundCommand out;
    if (command.empty()) return out;
    auto sh = shell(command);
    const auto program = program_of(sh.argv.front());
    if (!program) return out;
    os::process::spawn_options o;
    o.program = *program;
    o.argv0 = sh.argv.front();
    o.args.assign(sh.argv.begin() + 1, sh.argv.end());
    o.cwd = std::string(cwd);
    o.env = child_env({}, false);
    o.in = os::process::stdio::null;
    o.out = o.err = inheritStdio ? os::process::stdio::inherit : os::process::stdio::null;
    o.own_unit = true;
    o.bound_lifetime = true;
    o.one_string = sh.one_string;
    auto p = Process::spawn(o);
    if (!p) return out;
    auto state = std::make_shared<Background>();
    state->process = std::move(*p);
    out.ok = true;
    out.state = state;
    return out;
}

bool background_running(const BackgroundCommand& child, int* exitCode) {
    if (!child.ok) return false;
    auto* b = static_cast<Background*>(child.state.get());
    auto w = b->process->try_wait();
    if (!w || !w->has_value()) return w.has_value();
    if (exitCode) *exitCode = (*w)->shell_code();
    return false;
}

void stop_background(const BackgroundCommand& child, std::chrono::milliseconds grace) {
    if (!child.ok) return;
    auto* b = static_cast<Background*>(child.state.get());
    b->process->terminate();
    auto w = b->process->wait_for(grace);
    if (!w || !w->has_value()) {
        b->process->end_unit();
        b->process->kill();
    } else {
        b->process->end_unit();
    }
    (void)b->process->wait();
}

void guard_background_on_signal(const BackgroundCommand& child) {
    if (!child.ok) return;
    auto* b = static_cast<Background*>(child.state.get());
    if (!b->guard) b->guard.emplace(&*b->process);
}

void clear_background_guard(const BackgroundCommand& child) {
    if (!child.ok) return;
    static_cast<Background*>(child.state.get())->guard.reset();
}

}  // namespace mcpp::run
