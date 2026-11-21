// mcpp.build.test_fixture — the run fixtures `mcpp test` serves (SPEC-017 O6, P11; design
// 2026-11-20 §5.10).
//
// A run fixture is one object of the whole run: a sandboxed home, a server, an index. A program
// declares it (a `component` event with `fixture`), its cases name it in `uses`. The runner starts
// a program that declares it, with MTEST_SERVE naming it and its input a pipe, before the first
// program whose cases use it; reads `mtest-ready <endpoint>` from its output; gives the endpoint to
// each user as MTEST_FIXTURE_<NAME>; and closes the input after the last user ended, which ends the
// fixture. Its preparation is recorded on the fixture (`fixtures.ndjson`), not on whichever case
// happened to run first.

export module mcpp.build.test_fixture;

import std;
import mcpplibs.os.process;
import mcpp.build.test_protocol;

export namespace mcpp::build::test_fixture {

// How a program is started: what the runner starts for its cases, without the protocol's variables.
struct Provider {
    std::string                                      program;   // its name in the run
    std::vector<std::string>                         argv;
    std::vector<std::pair<std::string, std::string>> env;
    std::filesystem::path                            cwd;
};

// One fixture of the run, as `fixtures.ndjson` states it: milliseconds after the run began.
struct Run {
    std::string name;
    std::string program;
    std::string status;   // ready, failed, or unused when no user acquired it
    double      startedMs = 0;
    double      readyMs = 0;
    double      endedMs = 0;
    std::string error;
};

// MTEST_FIXTURE_<NAME>: the name upper-cased, each character not a letter or digit an underscore.
std::string variable(std::string_view name);

class Server {
public:
    // `providers`: the program that serves each fixture; `users`: how many programs of the run use
    // each, so that the last to release it ends it. `limit`: how long a fixture may take to be ready.
    Server(std::map<std::string, Provider> providers, std::map<std::string, int> users,
           std::chrono::steady_clock::time_point origin, std::chrono::milliseconds limit);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // The endpoint of `name`, started on the first call; a caller that arrives while another starts
    // it waits for the outcome. The error names the fixture and why it is not ready (P11.4).
    std::expected<std::string, std::string> acquire(const std::string& name);
    // One user of `name` ended; the last ends the fixture.
    void release(const std::string& name);
    // Ends every fixture still served and states each.
    std::vector<Run> finish();

private:
    struct Served {
        bool                                       started = false;
        bool                                       settled = false;
        Run                                        run;
        std::string                                endpoint;
        std::optional<mcpplibs::os::process::process> process;
    };
    double now() const;
    void   start(const Provider& p, Served& s);
    void   end(Served& s);

    std::map<std::string, Provider>         providers_;
    std::map<std::string, int>              users_;
    std::chrono::steady_clock::time_point   origin_;
    std::chrono::milliseconds               limit_;
    std::mutex                              m_;
    std::condition_variable                 cv_;
    std::map<std::string, Served>           served_;
};

}  // namespace mcpp::build::test_fixture

namespace mcpp::build::test_fixture {

namespace proc = mcpplibs::os::process;

std::string variable(std::string_view name) {
    std::string key = "MTEST_FIXTURE_";
    for (char c : name)
        key += std::isalnum(static_cast<unsigned char>(c)) ? char(std::toupper(static_cast<unsigned char>(c))) : '_';
    return key;
}

Server::Server(std::map<std::string, Provider> providers, std::map<std::string, int> users,
               std::chrono::steady_clock::time_point origin, std::chrono::milliseconds limit)
    : providers_(std::move(providers)), users_(std::move(users)), origin_(origin), limit_(limit) {}

Server::~Server() { (void)finish(); }

double Server::now() const {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - origin_).count();
}

std::expected<std::string, std::string> Server::acquire(const std::string& name) {
    std::unique_lock lock(m_);
    auto& s = served_[name];
    if (!s.started) {
        s.started = true;
        s.run.name = name;
        auto p = providers_.find(name);
        if (p == providers_.end()) {
            s.run.status = "failed";
            s.run.error = std::format("no program of the run declares the run fixture {}", name);
            s.settled = true;
        } else {
            s.run.program = p->second.program;
            // Started outside the lock: a fixture that takes minutes holds back no other fixture.
            lock.unlock();
            Served fresh;
            fresh.run = s.run;
            start(p->second, fresh);
            lock.lock();
            auto& at = served_[name];
            at.run = fresh.run;
            at.endpoint = std::move(fresh.endpoint);
            at.process = std::move(fresh.process);
            at.settled = true;
        }
        cv_.notify_all();
    }
    cv_.wait(lock, [&] { return served_[name].settled; });
    auto const& done = served_[name];
    if (done.run.status != "ready") return std::unexpected(done.run.error);
    return done.endpoint;
}

void Server::start(const Provider& p, Served& s) {
    s.run.startedMs = now();
    proc::spawn_options o;
    o.program = p.argv.front();
    o.args.assign(p.argv.begin() + 1, p.argv.end());
    o.cwd = p.cwd;
    o.in = proc::stdio::pipe;
    o.out = proc::stdio::pipe;
    o.err = proc::stdio::inherit;
    // Its own unit, which `end` ends. Not `bound_lifetime`: on Linux that binds the fixture to the
    // thread that started it, a worker, which ends before the fixture's last user does (measured).
    o.own_unit = true;
    for (auto const& [k, v] : p.env) o.env.emplace_back(k, v);
    // The program serves; it runs no case (P11.3), whatever this runner's own environment holds.
    for (auto v : mcpp::build::test_protocol::kVariables) o.env.emplace_back(std::string(v), std::nullopt);
    o.env.emplace_back("MTEST_SERVE", s.run.name);
    auto spawned = proc::process::spawn(o);
    if (!spawned) {
        s.run.status = "failed";
        s.run.error = std::format("the run fixture {} did not start: {}", s.run.name, spawned.error().message());
        return;
    }
    const auto deadline = std::chrono::steady_clock::now() + limit_;
    std::string text;
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) break;
        auto chunk = spawned->read_output_for(left);
        if (!chunk || !*chunk || (*chunk)->empty()) break;
        text += **chunk;
        for (auto nl = text.find('\n'); nl != std::string::npos; nl = text.find('\n')) {
            std::string line = text.substr(0, nl);
            text.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.starts_with("mtest-ready ")) continue;
            s.endpoint = line.substr(12);
            s.run.status = "ready";
            s.run.readyMs = now();
            s.process = std::move(*spawned);
            return;
        }
    }
    spawned->kill();
    (void)spawned->wait();
    s.run.status = "failed";
    s.run.endedMs = now();
    s.run.error = std::chrono::steady_clock::now() >= deadline
        ? std::format("the run fixture {} was not ready within {} s", s.run.name, limit_.count() / 1000)
        : std::format("the run fixture {} ended before it was ready", s.run.name);
}

void Server::end(Served& s) {
    if (!s.process) return;
    s.process->close_input();
    // A fixture that does not end when its input ends is ended, after the time it had to be ready.
    if (auto ended = s.process->wait_for(limit_); !ended || !*ended) {
        s.process->kill();
        (void)s.process->wait();
    }
    s.process->end_unit();
    s.process.reset();
    s.run.endedMs = now();
}

void Server::release(const std::string& name) {
    std::lock_guard lock(m_);
    if (--users_[name] > 0) return;
    if (auto it = served_.find(name); it != served_.end()) end(it->second);
}

std::vector<Run> Server::finish() {
    std::lock_guard lock(m_);
    std::vector<Run> out;
    for (auto& [name, s] : served_) {
        end(s);
        out.push_back(s.run);
    }
    return out;
}

}  // namespace mcpp::build::test_fixture
