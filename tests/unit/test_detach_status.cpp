#include <gtest/gtest.h>

import std;
import mcpp.build.schedule.detach_codegen;
import mcpp.run;
import mcpp.invocation;
import mcpplibs.os.fs;
import mcpplibs.os.platform;
import mcpplibs.os.process;

// A detached compile reports its status in `<slot>.rc`, which appears by a
// rename once the number is written. One that holds no number is a write that
// failed (a full disk); the waiter takes the unit as failed at once instead of
// waiting for a number that will never come.

namespace fs = std::filesystem;
namespace detach = mcpp::build::schedule::detach;

namespace {
struct Slot {
    fs::path dir, slot;
    Slot() {
        dir = fs::temp_directory_path() / std::format("mcpp-detach-{:08x}", std::random_device{}());
        fs::create_directories(dir);
        slot = dir / "unit.m.o.sched";
        std::ofstream(slot.string() + ".log") << "";
    }
    ~Slot() { std::error_code ec; fs::remove_all(dir, ec); }
};
}  // namespace

TEST(DetachStatus, AStatusFileWithNoNumberIsAFailedUnit) {
    Slot s;
    std::ofstream(s.slot.string() + ".rc") << "";
    const auto started = std::chrono::steady_clock::now();
    EXPECT_NE(detach::await_unit(s.slot, {}), 0);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(5));
}

TEST(DetachStatus, AWrittenStatusIsTheUnits) {
    Slot s;
    std::ofstream(s.slot.string() + ".rc") << "3\n";
    EXPECT_EQ(detach::await_unit(s.slot, {}), 3);
    Slot t;
    std::ofstream(t.slot.string() + ".rc") << "0\n";
    EXPECT_EQ(detach::await_unit(t.slot, {}), 0);
}

TEST(DetachStatus, OrdinaryCompilerStartFailureReleasesItsPermit) {
    Slot s;
    const auto semaphore = s.dir / "permits";
    EXPECT_EQ(detach::run_capped({(s.dir / "missing-compiler").string()}, semaphore, 1), 127);
    auto free = mcpplibs::os::fs::lock::acquire(semaphore / "slot-0.lock",
        mcpplibs::os::fs::lock::kind::exclusive, false);
    EXPECT_TRUE(free);
}

TEST(DetachStatus, AnUnavailableSemaphoreRefusesUnboundedCompilation) {
    Slot s;
    const auto semaphore = s.dir / "not-a-directory";
    std::ofstream(semaphore) << "occupied";
    EXPECT_NE(detach::run_capped({"unused-compiler"}, semaphore, 1), 0);
    EXPECT_TRUE(fs::is_regular_file(semaphore));
}


TEST(DetachStatus, KernelSlotsDistinguishContentionFromFailure) {
    Slot s;
    const auto path = s.dir / "slot.lock";
    {
        auto held = mcpplibs::os::fs::lock::acquire(path,
            mcpplibs::os::fs::lock::kind::exclusive, false);
        ASSERT_TRUE(held);
        auto busy = mcpplibs::os::fs::lock::acquire(path,
            mcpplibs::os::fs::lock::kind::exclusive, false);
        ASSERT_FALSE(busy);
        EXPECT_EQ(busy.error().code, mcpplibs::os::errc::again);
    }
    EXPECT_TRUE(fs::exists(path));
    auto reused = mcpplibs::os::fs::lock::acquire(path,
        mcpplibs::os::fs::lock::kind::exclusive, false);
    EXPECT_TRUE(reused);
}

namespace {
std::string lifetime_env(std::string_view key, std::string fallback = {}) {
    return mcpp::invocation::get(key).value_or(std::move(fallback));
}
int lifetime_driver() {
    const auto role = lifetime_env("MCPP_TEST_LIFETIME_ROLE");
    const auto self = mcpp::invocation::self_exe_path();
    const auto argv = ::testing::internal::GetArgvs();
    if (argv.size() > 1 && argv[1] == "bmi-supervise") {
        auto value = [&](std::string_view key) {
            auto it = std::ranges::find(argv, key);
            return it != argv.end() && ++it != argv.end() ? *it : std::string{};
        };
        const auto cmd = value("--command-file");
        std::ifstream in(cmd);
        std::string command{std::istreambuf_iterator<char>(in), {}};
        return detach::supervise(value("--slot"), value("--sem"),
            std::stoi(value("--cap")), value("--object"), command);
    }
    if (role == "root") {
        // The generation's cap; build.ninja names none (e2e 861).
        auto run = detach::RunScope::begin(lifetime_env("MCPP_TEST_LIFETIME_OUTPUT", fs::current_path().string()),
                                           std::stoi(lifetime_env("MCPP_TEST_LIFETIME_CAP", "1")));
        if (!run) { std::println(std::cerr, "{}", run.error()); return 2; }
        std::ofstream("root.ready") << mcpplibs::os::process::self()->id;
        mcpp::run::Env env;
        run->environment(env);
        // Test an owner whose process is alive but whose drive has finished.
        std::jthread closer;
        if (lifetime_env("MCPP_TEST_LIFETIME_CLOSE") == "1") {
            closer = std::jthread([&] {
                while (!fs::exists("compiler.ready"))
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                auto done = run->finish();
                std::ofstream("owner.closed") << (done ? "closed" : done.error());
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
            });
        }
        auto result = mcpp::run::execute({"ninja", "-j"+lifetime_env("MCPP_TEST_LIFETIME_NINJA_JOBS","12")}, {
            .env = env, .should_stop = [&] { return run->failed(); }});
        std::cout<<result.output;
        if (closer.joinable()) closer.join();
        auto done = run->finish();
        return done ? result.exit_code : 2;
    }
    if (role == "phase") {
        std::ofstream("phase.pid") << mcpplibs::os::process::self()->id;
        detach::CompileRequest request;
        request.bmi = "published.gcm"; request.object = "module.o";
        request.slot = "unit"; request.self = self;
        request.semaphore = "permits"; request.maxCompilers = 1;
        request.commandFile = "compile.command";
        std::ifstream in(request.commandFile);
        request.command.assign(std::istreambuf_iterator<char>(in), {});
        return detach::compile_release_at_bmi(request);
    }
    if (role == "await") return detach::await_unit("unit", "module.o");
    if (role == "ordinary")
        return detach::run_capped({"env", "MCPP_TEST_LIFETIME_ROLE=compiler", self.string()},
                                  "permits", 1, lifetime_env("MCPP_TEST_LIFETIME_OBJECT","ordinary.o"));
    if (role == "compiler" && lifetime_env("MCPP_TEST_LIFETIME_DESCENDANT") == "1") {
        const auto who=mcpplibs::os::process::self();
        std::ofstream("compiler.ready")<<who->id<<' '<<who->started_ns;
        mcpplibs::os::process::spawn_options options;
        options.program=self;options.env={{"MCPP_TEST_LIFETIME_ROLE","writer"}};
        options.out=options.err=mcpplibs::os::process::stdio::inherit;
        auto writer=mcpplibs::os::process::process::spawn(options);
        if(!writer)return 2;
        auto result=writer->wait();return result?result->shell_code():2;
    }
    if (role == "compiler" || role == "writer") {
        const auto process = mcpplibs::os::process::self();
        std::ofstream(role=="writer"?"writer.ready":"compiler.ready") << process->id << ' ' << process->started_ns;
        std::ofstream("compiler.runs", std::ios::app) << process->id << '\n';
        const bool ordinary = lifetime_env("MCPP_TEST_LIFETIME_KIND") == "ordinary";
        const auto object = ordinary ? lifetime_env("MCPP_TEST_LIFETIME_OBJECT","ordinary.o") : "module.o";
        std::ofstream(object) << "partial";
        const int fail = std::stoi(lifetime_env("MCPP_TEST_LIFETIME_FAIL", "0"));
        const auto publicationGate=lifetime_env("MCPP_TEST_LIFETIME_PUBLICATION_GATE");
        while(!publicationGate.empty()&&!fs::exists(publicationGate))
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (!ordinary && fail != 19) {
            std::ofstream("published.gcm.new") << "same BMI";
            fs::rename("published.gcm.new", "published.gcm");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(
            std::stoi(lifetime_env("MCPP_TEST_LIFETIME_DELAY", "20000"))));
        if (!fail) std::ofstream(object) << "complete";
        if(role=="writer")std::ofstream("writer.finished")<<"committed";
        return fail;
    }
    return 2;
}
} // namespace

// This copy is an actual detached supervisor/phase1/ordinary compiler under
// Ninja. It is selected through GTEST_FILTER inherited by detached children.
TEST(DetachLifetime, SubprocessDriver) {
    if (lifetime_env("MCPP_TEST_LIFETIME_ROLE").empty()) GTEST_SKIP() << "subprocess driver";
    mcpp::run::install();
    const int rc = lifetime_driver();
    std::cout.flush(); std::cerr.flush();
    std::_Exit(rc);
}

TEST(DetachLifetime, OwnerCancellationAndImmediateRestart) {
    if constexpr (!mcpplibs::os::is_linux) GTEST_SKIP() << "Linux signal and LD_PRELOAD controls";
    Slot temporary;
    const auto helper = fs::current_path() / "tests/e2e/helpers/check_owned_schedule_lifetime.py";
    ASSERT_TRUE(fs::exists(helper));
    const auto result = mcpp::run::execute({"python3", helper.string(),
        mcpp::invocation::self_exe_path().string(), temporary.dir.string()});
    EXPECT_EQ(result.exit_code, 0) << result.output;
}

TEST(DetachLifetime, TransactionRecoveryAtNativeSyscallBoundaries) {
    if constexpr (!mcpplibs::os::is_linux
        || mcpplibs::os::ARCH != mcpplibs::os::architecture::x86_64)
        GTEST_SKIP() << "Linux x86-64 syscall boundary control";
    Slot temporary;
    const auto helper=fs::current_path()/"tests/e2e/helpers/check_compiler_transaction_faults.py";
    ASSERT_TRUE(fs::exists(helper));
    const auto result=mcpp::run::execute({"python3",helper.string(),
        mcpp::invocation::self_exe_path().string(),temporary.dir.string()},
        {.total=std::chrono::seconds(120)});
    EXPECT_EQ(result.exit_code,0)<<result.output;
}

TEST(DetachLifetime, ActualGccMixedCapacityUsesOwnedUnits) {
    if constexpr (!mcpplibs::os::is_linux)GTEST_SKIP()<<"native GCC and Linux counter locks";
    const auto engine=lifetime_env("MCPP_FRESH",lifetime_env("MCPP"));
    if(engine.empty())GTEST_SKIP()<<"requires the explicit fresh mcpp supplied by CI";
    ASSERT_TRUE(fs::is_regular_file(engine));
    Slot temporary;
    const auto helper=fs::current_path()/"tests/e2e/helpers/check_shared_compiler_cap.py";
    const auto result=mcpp::run::execute({"python3",helper.string(),engine,(temporary.dir/"managed-gcc").string(),
        "--owner-driver",mcpp::invocation::self_exe_path().string()},
        {.total=std::chrono::seconds(120)});
    EXPECT_EQ(result.exit_code,0)<<result.output;
}
