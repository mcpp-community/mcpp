// What openkal answers inside an ordinary hosted program: the host's C and C++
// runtime own the process (glibc / libSystem / UCRT, libstdc++ or libc++ or the
// MSVC STL), and an openkal implementation is linked in its default
// arrangement, borrowing that runtime. Each line states one fact the `os`
// library would rest on. The probe prints and never asserts; the exit status is
// 0 unless the probe itself could not run.
//
// The child is this program again (`okprobe child <mode> ...`), so no shell or
// system utility is assumed on any platform.
import std;
import openkal.types;
import openkal.stream;
import openkal.env;
import openkal.fs;
import openkal.process;
import openkal.terminal;
import openkal.net;
import openkal.time;
import openkal.task;
import openkal.timeout;
import openkal.macros;
import openkal.version;

using namespace kal::macros;

namespace {

void fact(std::string_view area, std::string_view item, std::string_view value) {
    std::println("PROBE {:<9} {:<34} {}", area, item, value);
    std::cout.flush();
}
std::string err(int rc) { return rc == 0 ? std::string("ok") : std::format("error {}", rc); }
std::string bits(kal_uintptr word, std::initializer_list<std::pair<kal_uintptr, const char*>> names) {
    std::string out;
    for (auto [b, n] : names) if (word & b) { if (!out.empty()) out += ' '; out += n; }
    return out.empty() ? "(none)" : out;
}

struct Preopen { kal_dir dir; std::string name; };
std::vector<Preopen> preopens() {
    std::vector<Preopen> out;
    for (kal_uintptr i = 0; i < kal_fs_preopen_count(); ++i) {
        kal_dir d{}; char n[1024]; kal_uintptr l = 0;
        if (kal_fs_preopen(i, &d, n, sizeof n, &l) == 0) out.push_back({d, std::string(n, l)});
    }
    return out;
}

// An absolute host path as openkal names it: the preopen whose name is the
// longest prefix, and the rest with forward slashes and no leading separator.
// This is the mapping the `os` library would own.
std::optional<std::pair<kal_dir, std::string>> locate(const std::vector<Preopen>& pre, std::filesystem::path p) {
    auto text = p.lexically_normal().generic_string();
    const Preopen* best = nullptr;
    for (const auto& o : pre) {
        auto name = std::filesystem::path(o.name).generic_string();
        if (!text.starts_with(name)) continue;
        if (text.size() > name.size() && text[name.size()] != '/' && !name.ends_with('/')) continue;
        if (!best || name.size() > std::filesystem::path(best->name).generic_string().size()) best = &o;
    }
    if (!best) return std::nullopt;
    auto rest = text.substr(std::filesystem::path(best->name).generic_string().size());
    while (!rest.empty() && rest.front() == '/') rest.erase(0, 1);
    if (rest.empty()) rest = ".";
    return std::pair{best->dir, rest};
}

std::string self_path() {
    char buf[4096];
    const auto n = kal_env_arg(0, buf, sizeof buf);
    std::filesystem::path a = n > 0 ? std::string(buf, static_cast<std::size_t>(n)) : std::string{};
    if (a.is_relative()) a = std::filesystem::current_path() / a;
    std::error_code ec;
    if (!std::filesystem::exists(a, ec) && std::filesystem::exists(a.string() + ".exe", ec)) a += ".exe";
    return a.string();
}

std::string read_all(kal_stream s) {
    std::string out; char buf[4096];
    for (;;) { const auto n = kal_stream_read(s, buf, sizeof buf); if (n <= 0) break; out.append(buf, static_cast<std::size_t>(n)); }
    return out;
}

struct Child {
    kal_process proc{};
    kal_stream out{}, err{}, in{};
    int rc = -1;
};

// Spawns this program as `child <args...>` with pipes on all three streams.
Child spawn_self(const std::vector<Preopen>& pre, std::vector<std::string> args, kal_job* job,
                 std::vector<std::string> env = {}, std::optional<kal_dir> work = {}, kal_uintptr flags = 0) {
    Child c;
    const auto self = self_path();
    auto at = locate(pre, self);
    if (!at) { c.rc = -100; return c; }
    kal_stream outTheirs{}, errTheirs{}, inTheirs{};
    kal_process_channel(&c.out, &outTheirs);
    kal_process_channel(&c.err, &errTheirs);
    // openkal's channel is directional: `mine` is the reading end. A child's
    // input is therefore the pair reversed -- the child receives `mine` and the
    // parent keeps `theirs`, the writing end.
    kal_process_channel(&inTheirs, &c.in);
    args.insert(args.begin(), {self, "child"});
    std::vector<const char*> argv; std::vector<kal_uintptr> lens;
    for (auto& a : args) { argv.push_back(a.c_str()); lens.push_back(a.size()); }
    // The environment is passed whole, as `os` would build it: the host's, plus overrides.
    std::vector<std::string> envs;
    for (kal_uintptr i = 0; i < kal_env_var_count(); ++i) {
        char b[32768]; const auto n = kal_env_var_at(i, b, sizeof b);
        if (n > 0) envs.emplace_back(b, static_cast<std::size_t>(n));
    }
    for (auto& e : env) envs.push_back(e);
    std::vector<const char*> envp; std::vector<kal_uintptr> elens;
    for (auto& e : envs) { envp.push_back(e.c_str()); elens.push_back(e.size()); }
    kal_spawn how{};
    how.base = at->first;
    how.work = work ? *work : pre.front().dir;
    how.job = job;
    how.flags = flags;
    kal_spawn_streams st{inTheirs, outTheirs, errTheirs};
    c.rc = kal_process_spawn(&how, at->second.c_str(), at->second.size(), argv.data(), lens.data(), argv.size(),
                             envp.data(), elens.data(), envp.size(), &st, &c.proc);
    kal_process_channel_close(outTheirs);
    kal_process_channel_close(errTheirs);
    kal_process_channel_close(inTheirs);
    return c;
}

// ── the child ─────────────────────────────────────────────────────────────
int child(std::vector<std::string> a) {
    const auto mode = a.empty() ? std::string{} : a[0];
    auto put = [](std::string_view s) { kal_stream_write(kal_stdout(), s.data(), s.size()); };
    if (mode == "echo") {
        char v[256]; const auto n = kal_env_var("PROBE_X", 7, v, sizeof v);
        std::string input = read_all(kal_stdin());
        put(std::format("out:{} env:{} stdin:{} cwd:{}", a.size() > 1 ? a[1] : "", n >= 0 ? std::string(v, n) : "<absent>",
                        input, std::filesystem::current_path().filename().string()));
        kal_stream_write(kal_stderr(), "err-bytes", 9);
        return 3;
    }
    if (mode == "sleep") { std::this_thread::sleep_for(std::chrono::milliseconds(std::stoi(a.at(1)))); return 0; }
    if (mode == "stop") {
        const kal_u32* word = kal_process_stop_requested();
        put(word ? "armed\n" : "not-armed\n");
        if (!word) return 9;
        const auto t0 = std::chrono::steady_clock::now();
        while (*word == 0 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5))
            kal_task_wait(word, 0, 100'000'000);
        put(*word ? "noticed\n" : "timeout\n");
        return *word ? 7 : 8;
    }
    if (mode == "stop-raise") {
        // The request delivered by the host runtime's own raise(): isolates the
        // handler from the waiting.
        const kal_u32* word = kal_process_stop_requested();
        if (!word) { put("not-armed\n"); return 9; }
        std::raise(15);
        put(*word ? "noticed\n" : "missed\n");
        return *word ? 7 : 8;
    }
    if (mode == "tree") {
        // Starts a grandchild that writes `marker` after a delay, outside any
        // unit of this child's making, then sleeps. A unit terminated from the
        // top must take the grandchild too.
        auto pre = preopens();
        auto at = locate(pre, a.at(1));
        std::string marker = a.at(1);
        Child g = spawn_self(pre, {"late-write", marker}, nullptr);
        put(std::format("grandchild:{}\n", err(g.rc)));
        std::this_thread::sleep_for(std::chrono::seconds(5));
        return 0;
    }
    if (mode == "late-write") {
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        std::ofstream(a.at(1)) << "alive";
        return 0;
    }
    return 64;
}

// ── the parent ────────────────────────────────────────────────────────────
void probe_identity() {
    fact("host", "program", std::filesystem::path(self_path()).filename().string());
    fact("openkal", "contract version", std::format("{:#x}", kal_version()));
    fact("openkal", "interfaces word", std::format("{:#x}", kal_interfaces()));
}

void probe_env(const std::vector<Preopen>& pre) {
    for (std::size_t i = 0; i < pre.size(); ++i) fact("fs", std::format("preopen {}", i), pre[i].name);
    fact("env", "argv[0] (self, as given)", self_path());
    char v[4096];
    for (const char* name : {"PATH", "HOME", "USERPROFILE", "APPDATA", "XDG_CONFIG_HOME"}) {
        const auto n = kal_env_var(name, std::strlen(name), v, sizeof v);
        fact("env", name, n >= 0 ? std::format("{} bytes", n) : "<absent>");
    }
    fact("env", "count", std::to_string(kal_env_var_count()));
    fact("env", "host std::getenv(PATH) agrees",
         std::getenv("PATH") && kal_env_var("PATH", 4, v, sizeof v) == static_cast<kal_intptr>(std::strlen(std::getenv("PATH"))) ? "yes" : "no");
}

void probe_fs(const std::vector<Preopen>& pre) {
    const auto base = std::filesystem::temp_directory_path() / std::format("okprobe-{}", std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(base);
    auto at = locate(pre, base);
    fact("fs", "temp dir reachable via preopen", at ? std::format("{} + {}", "preopen", at->second) : "NO");
    if (!at) return;
    kal_dir d{};
    int rc = kal_fs_open_dir(at->first, at->second.c_str(), at->second.size(), &d);
    fact("fs", "open_dir(temp)", err(rc));
    if (rc) return;
    const auto props = kal_fs_props(d);
    fact("fs", "props(temp volume)", bits(props, {{KAL_FS_PROP_CASE_SENSITIVE_M, "case-sensitive"}, {KAL_FS_PROP_LINKS_M, "links"},
        {KAL_FS_PROP_MODIFIED_TIME_M, "mtime"}, {KAL_FS_PROP_ATOMIC_RENAME_M, "atomic-rename"}, {KAL_FS_PROP_MAKE_LINKS_M, "make-links"},
        {KAL_FS_PROP_LOCKS_M, "locks"}, {KAL_FS_PROP_CAPACITY_M, "capacity"}, {KAL_FS_PROP_EXECUTABLE_M, "executable"}}));
    kal_file f{};
    rc = kal_fs_open(d, "a.tmp", 5, KAL_OPEN_WRITE_M | KAL_OPEN_CREATE_M | KAL_OPEN_EXCLUSIVE_M, &f);
    fact("fs", "open(create|exclusive)", err(rc));
    if (!rc) {
        kal_stream s = kal_fs_stream(f);
        kal_stream_write(s, "hello", 5);
        fact("fs", "flush (durability)", err(kal_stream_flush(s)));
        kal_fs_close_file(f);
    }
    rc = kal_fs_open(d, "a.tmp", 5, KAL_OPEN_WRITE_M | KAL_OPEN_CREATE_M | KAL_OPEN_EXCLUSIVE_M, &f);
    fact("fs", "open(create|exclusive) again", err(rc));
    { kal_file g{}; kal_fs_open(d, "b", 1, KAL_OPEN_WRITE_M | KAL_OPEN_CREATE_M, &g); kal_fs_close_file(g); }
    fact("fs", "rename onto existing (replaces)", err(kal_fs_rename(d, "a.tmp", 5, d, "b", 1)));
    fact("fs", "host sees replaced content", [&] { std::ifstream in(base / "b"); std::string s; in >> s; return s; }());
    fact("fs", "rename without replacing", "no operation in openkal.fs (kal_fs_rename has no flags)");
    kal_node_info info{}; info.self_size = sizeof info;
    rc = kal_fs_info(d, "b", 1, 0, KAL_INFO_ALL_M, &info);
    fact("fs", "info(b)", std::format("{} present={:#x} size={} kind={} identity={:x}:{:x}", err(rc), info.present, info.size, info.kind,
                                      info.identity[0], info.identity[1]));
    fact("fs", "set_executable_at(b)", err(kal_fs_set_executable_at(d, "b", 1, 1)));
    { const auto m = err(kal_fs_mkdir(d, "sub", 3)); const auto r = err(kal_fs_remove(d, "sub", 3)); fact("fs", "mkdir(sub) / remove(sub)", m + " / " + r); }
    fact("fs", "name with '..' refused", err(kal_fs_mkdir(d, "x/../y", 6)));
    // Locks: two handles of this process, then a child would be the cross-process case.
    kal_file l1{}, l2{};
    kal_fs_open(d, "lock", 4, KAL_OPEN_WRITE_M | KAL_OPEN_CREATE_M, &l1);
    kal_fs_open(d, "lock", 4, KAL_OPEN_WRITE_M, &l2);
    fact("fs", "lock exclusive (handle 1)", err(kal_fs_lock(l1, 0, 1, KAL_LOCK_EXCLUSIVE_M)));
    fact("fs", "lock exclusive (handle 2, no wait)", err(kal_fs_lock(l2, 0, 1, KAL_LOCK_EXCLUSIVE_M)));
    kal_fs_close_file(l1); kal_fs_close_file(l2);
    kal_u64 total = 0, avail = 0;
    { const auto r = err(kal_fs_capacity(d, &total, &avail)); fact("fs", "capacity", std::format("{} total={}MiB available={}MiB", r, total >> 20, avail >> 20)); }
    rc = kal_fs_link_create(d, "ln", 2, "b", 1, 0);
    fact("fs", "link_create(file)", err(rc));
    { char t[256]; const auto n = kal_fs_link_read(d, "ln", 2, t, sizeof t); fact("fs", "link_read", n >= 0 ? std::string(t, n) : err(static_cast<int>(-n))); }
    std::filesystem::create_directories(base / "dir");
    fact("fs", "link_create(directory)", err(kal_fs_link_create(d, "dln", 3, "dir", 3, KAL_FS_LINK_DIRECTORY_M)));
    kal_uintptr it = 0; int count = 0;
    if (kal_fs_list_begin(d, &it) == 0) {
        char n[512]; kal_uintptr nl = 0; int kind = 0;
        while (it && kal_fs_list_next(d, &it, n, sizeof n, &nl, &kind) == 0 && it) ++count;
    }
    fact("fs", "list entries", std::to_string(count));
    kal_fs_close_dir(d);
    std::error_code ec; std::filesystem::remove_all(base, ec);
}

void probe_process(const std::vector<Preopen>& pre) {
    fact("process", "props", bits(kal_process_props(), {{KAL_PROCESS_PROP_TERMINATE_M, "terminate"}, {KAL_PROCESS_PROP_STREAM_PASSING_M, "streams"},
        {KAL_PROCESS_PROP_EXIT_STATUS_M, "exit-status"}, {KAL_PROCESS_PROP_CHANNEL_M, "channel"}, {KAL_PROCESS_PROP_GRANT_DIR_M, "grant-dir"},
        {KAL_PROCESS_PROP_BOUND_LIFETIME_M, "bound-lifetime"}, {KAL_PROCESS_PROP_JOB_M, "job"}, {KAL_PROCESS_PROP_STOP_REQUESTED_M, "stop-requested"}}));

    // argv with spaces and quotes, an environment override, stdin, a working directory.
    const auto work = std::filesystem::temp_directory_path();
    auto w = locate(pre, work);
    std::optional<kal_dir> wd;
    if (w) { kal_dir d{}; if (kal_fs_open_dir(w->first, w->second.c_str(), w->second.size(), &d) == 0) wd = d; }
    Child c = spawn_self(pre, {"echo", "a b \"c\" \\d"}, nullptr, {"PROBE_X=over ride"}, wd);
    fact("process", "spawn(self, pipes, env, cwd)", err(c.rc));
    if (c.rc == 0) {
        kal_stream_write(c.in, "to-child", 8);
        kal_process_channel_close(c.in);
        std::string err_out;
        std::jthread t([&] { err_out = read_all(c.err); });
        auto out = read_all(c.out);
        t.join();
        int status = -1, term = -1;
        kal_process_wait(c.proc, &status, &term);
        fact("process", "child stdout", out);
        fact("process", "child stderr", err_out);
        fact("process", "status / terminated", std::format("{} / {}", status, term));
        kal_process_close(c.proc);
    }

    // try_wait: a bound of 1 ns on a running child.
    Child s = spawn_self(pre, {"sleep", "300"}, nullptr);
    if (s.rc == 0) {
        int st = 0, tm = 0;
        const int r1 = kal_timeout_wait_process(s.proc, 1, &st, &tm);
        const int r2 = kal_timeout_wait_process(s.proc, 5'000'000'000ull, &st, &tm);
        fact("process", "timeout_wait 1ns while running", err(r1));
        fact("process", "timeout_wait 5s until exit", std::format("{} status={}", err(r2), st));
        kal_process_close(s.proc);
    }

    // A request to end, observed by the child through the stop word.
    Child p = spawn_self(pre, {"stop"}, nullptr);
    if (p.rc == 0) {
        char b[32]; const auto n = kal_stream_read(p.out, b, sizeof b);
        fact("process", "child stop word", n > 0 ? std::string(b, n - 1) : "no output");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        fact("process", "terminate(child)", err(kal_process_terminate(p.proc)));
        auto rest = read_all(p.out);
        int st = 0, tm = 0;
        kal_process_wait(p.proc, &st, &tm);
        fact("process", "child after terminate", std::format("said='{}' status={} terminated={}",
             rest.empty() ? "" : rest.substr(0, rest.size() - 1), st, tm));
        kal_process_close(p.proc);
    }

    if constexpr (true) {
        Child r = spawn_self(pre, {"stop-raise"}, nullptr);
        if (r.rc == 0) {
            auto said = read_all(r.out);
            int st = 0, tm = 0; kal_process_wait(r.proc, &st, &tm);
            fact("process", "stop word via raise(SIGTERM)", std::format("said='{}' status={} terminated={}",
                 said.empty() ? "" : said.substr(0, said.size() - 1), st, tm));
            kal_process_close(r.proc);
        }
    }

    // A unit: terminating it takes a grandchild the caller never held.
    const auto marker = std::filesystem::temp_directory_path() / std::format("okprobe-marker-{}", std::chrono::steady_clock::now().time_since_epoch().count());
    kal_job job{};
    Child t = spawn_self(pre, {"tree", marker.string()}, &job);
    fact("process", "spawn into new unit", err(t.rc) + std::format(" job={}", job.h != 0 ? "set" : "unset"));
    if (t.rc == 0) {
        char b[64]; kal_stream_read(t.out, b, sizeof b);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        fact("process", "job_terminate", err(kal_process_job_terminate(job)));
        int st = 0, tm = 0;
        kal_process_wait(t.proc, &st, &tm);
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));
        std::error_code ec;
        fact("process", "grandchild survived the unit", std::filesystem::exists(marker, ec) ? "YES (escaped)" : "no");
        std::filesystem::remove(marker, ec);
        kal_process_close(t.proc);
        kal_process_job_close(job);
    }

    // Concurrent spawns from four host threads: a child of one thread must not
    // hold another's pipe, or that reader never sees the end of its stream.
    std::atomic<int> late{0};
    {
        std::vector<std::jthread> threads;
        for (int i = 0; i < 4; ++i)
            threads.emplace_back([&, i] {
                for (int k = 0; k < 5; ++k) {
                    Child q = spawn_self(pre, {i % 2 ? "sleep" : "echo", i % 2 ? "1500" : "x"}, nullptr);
                    if (q.rc) continue;
                    kal_process_channel_close(q.in);
                    const auto t0 = std::chrono::steady_clock::now();
                    if (i % 2 == 0) {
                        read_all(q.out);
                        if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(1000)) ++late;
                    }
                    int st = 0, tm = 0; kal_process_wait(q.proc, &st, &tm);
                    kal_process_close(q.proc);
                }
            });
    }
    fact("process", "concurrent spawns: EOF delayed by a sibling", std::to_string(late.load()));
}

void probe_terminal() {
    for (auto [name, s] : {std::pair{"stdin", kal_stdin()}, {"stdout", kal_stdout()}, {"stderr", kal_stderr()}}) {
        kal_uintptr cols = 0, rows = 0;
        const int rc = kal_terminal_size(s, &cols, &rows);
        fact("terminal", std::format("{} interactive / size", name),
             std::format("{} / {}", (kal_stream_props(s) & KAL_STREAM_PROP_INTERACTIVE_M) ? "yes" : "no",
                         rc == 0 ? std::format("{}x{}", cols, rows) : err(rc)));
    }
}

void probe_net() {
    kal_endpoint any{}; any.addr_len = 4; any.addr[0] = 127; any.addr[3] = 1; any.port = 0;
    kal_net_listener l{};
    int rc = kal_net_listen(&any, &l);
    fact("net", "listen 127.0.0.1:0", err(rc));
    if (rc) return;
    kal_endpoint at{};
    kal_net_listener_local(l, &at);
    std::jthread server([&] {
        kal_net_conn c{};
        if (kal_timeout_accept(l, 5'000'000'000ull, &c) == 0) { kal_stream_write(kal_net_stream(c), "pong", 4); kal_net_close(c); }
    });
    kal_net_conn c{};
    rc = kal_net_connect(&at, &c);
    std::string got = rc == 0 ? read_all(kal_net_stream(c)) : err(rc);
    fact("net", "loopback round trip", std::format("port={} got={}", at.port, got));
    if (rc == 0) kal_net_close(c);
    server.join();
    kal_net_close_listener(l);
    fact("net", "local (unix-domain / named pipe) endpoint", "no endpoint form in openkal.net (addr_len 4/16/20 only)");
}

void probe_task() {
    // A host std::thread waits on a word with kal_task_wait; another wakes it.
    kal_u32 word = 0;
    std::atomic<bool> woke{false};
    std::jthread waiter([&] { while (std::atomic_ref(word).load() == 0) kal_task_wait(&word, 0, 2'000'000'000ull); woke = true; });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::atomic_ref(word).store(1);
    kal_uintptr woken = 0; kal_task_wake(&word, 1, &woken);
    waiter.join();
    fact("task", "kal_task_wait/wake between std::threads", woke ? "yes" : "no");
    fact("task", "parallelism", std::to_string(kal_task_parallelism()));
    const auto t0 = kal_time_monotonic();
    kal_time_sleep(10'000'000);
    fact("time", "sleep 10ms measured", std::format("{:.1f}ms", (kal_time_monotonic() - t0) / 1e6));
}

void probe_self_location() {
    fact("env", "executable's own path", "no operation in openkal (kal_env_arg(0) is argv[0] as given)");
    fact("env", "set a variable for this process", "no operation in openkal (by design: no ambient mutable state)");
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (!args.empty() && args[0] == "child") return child({args.begin() + 1, args.end()});
    const auto pre = preopens();
    probe_identity();
    probe_env(pre);
    probe_self_location();
    probe_fs(pre);
    probe_process(pre);
    probe_terminal();
    probe_net();
    probe_task();
    std::println("PROBE done");
    return 0;
}
