#include <gtest/gtest.h>
#include <cerrno>
#include <fstream>

import std;
import mcpp.build.runner_lookup;

using namespace mcpp::build::runner_lookup;

// The lookup order is the whole point (#544 §4.4): a declared payload's bin/
// beats PATH, because a bare name on PATH reaches an xvm shim that answers for
// the current subos rather than for the package. The directories are real and
// the files are executable, so what is asserted is the rule, not a mock of it.
#if !defined(_WIN32)

namespace {
std::filesystem::path fresh_root(std::string_view name) {
    auto root = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}
std::filesystem::path make_exe(const std::filesystem::path& dir, std::string_view name) {
    std::filesystem::create_directories(dir);
    auto p = dir / name;
    { std::ofstream o(p); o << "#!/bin/sh\nexit 0\n"; }
    std::filesystem::permissions(p, std::filesystem::perms::owner_all);
    return p;
}
} // namespace

TEST(RunnerLookup, PayloadBinBeatsPath) {
    auto root = fresh_root("mcpp-runner-lookup-1");
    auto inPayload = make_exe(root / "payload" / "bin", "qemu-x");
    make_exe(root / "path", "qemu-x");
    std::vector<std::filesystem::path> bins{root / "payload" / "bin"};
    auto l = locate("qemu-x", bins, (root / "path").string());
    ASSERT_TRUE(l.program.has_value());
    EXPECT_EQ(*l.program, inPayload);
}

TEST(RunnerLookup, PathIsSearchedAfterPayloads) {
    auto root = fresh_root("mcpp-runner-lookup-2");
    auto onPath = make_exe(root / "path", "qemu-y");
    std::vector<std::filesystem::path> bins{root / "payload" / "bin"};   // absent dir
    auto l = locate("qemu-y", bins, (root / "path").string());
    ASSERT_TRUE(l.program.has_value());
    EXPECT_EQ(*l.program, onPath);
    ASSERT_EQ(l.searched.size(), 2u);
    EXPECT_EQ(l.searched[0], root / "payload" / "bin");
    EXPECT_EQ(l.searched[1], root / "path");
}

TEST(RunnerLookup, NonExecutableFileIsSkipped) {
    auto root = fresh_root("mcpp-runner-lookup-3");
    std::filesystem::create_directories(root / "p1");
    { std::ofstream o(root / "p1" / "tool"); o << "data"; }   // no exec bit
    auto real = make_exe(root / "p2", "tool");
    auto l = locate("tool", {}, (root / "p1").string() + ":" + (root / "p2").string());
    ASSERT_TRUE(l.program.has_value());
    EXPECT_EQ(*l.program, real);
}

TEST(RunnerLookup, NotFoundListsEveryDirectorySearched) {
    auto root = fresh_root("mcpp-runner-lookup-4");
    std::vector<std::filesystem::path> bins{root / "a" / "bin"};
    auto l = locate("nope", bins, (root / "p1").string() + ":" + (root / "p2").string());
    EXPECT_FALSE(l.program.has_value());
    ASSERT_EQ(l.searched.size(), 3u);
    auto msg = not_found_message("aarch64-linux-musl", "nope", l.searched);
    EXPECT_NE(msg.find("runner 'nope'"), std::string::npos) << msg;
    EXPECT_NE(msg.find("aarch64-linux-musl"), std::string::npos) << msg;
    EXPECT_NE(msg.find((root / "a" / "bin").string()), std::string::npos) << msg;
    EXPECT_NE(msg.find((root / "p2").string()), std::string::npos) << msg;
    EXPECT_NE(msg.find("[xlings] deps"), std::string::npos) << msg;
    EXPECT_NE(msg.find("--no-runner"), std::string::npos) << msg;
}

TEST(RunnerLookup, AbsoluteArgv0IsTakenAsIs) {
    auto root = fresh_root("mcpp-runner-lookup-5");
    auto abs = make_exe(root, "runner.sh");
    auto l = locate(abs.string(), {}, "");
    ASSERT_TRUE(l.program.has_value());
    EXPECT_EQ(*l.program, abs);
    // ...and an absolute path that is not there is not searched for elsewhere.
    auto missing = locate((root / "absent.sh").string(), {}, root.string());
    EXPECT_FALSE(missing.program.has_value());
}

TEST(RunnerLookup, EmptyPathEntriesAreIgnored) {
    auto root = fresh_root("mcpp-runner-lookup-6");
    auto onPath = make_exe(root / "p", "tool");
    auto l = locate("tool", {}, ":" + (root / "p").string() + "::");
    ASSERT_TRUE(l.program.has_value());
    EXPECT_EQ(*l.program, onPath);
    EXPECT_EQ(l.searched.size(), 1u);
}

TEST(RunnerLookup, ClassifiesENOEXECAsUnloadable) {
    EXPECT_EQ(classify(ENOEXEC), SpawnClass::Unloadable);
    EXPECT_EQ(classify(EACCES), SpawnClass::Other);
    EXPECT_EQ(classify(ENOENT), SpawnClass::Other);
    EXPECT_EQ(classify(0), SpawnClass::Other);
}

TEST(RunnerLookup, UnrunnableMessageNamesKernelAnswerTripleAndKey) {
    auto msg = unrunnable_message("aarch64-linux-musl", "/x/bin/app", ENOEXEC);
    EXPECT_NE(msg.find("Exec format error"), std::string::npos) << msg;
    EXPECT_NE(msg.find("/x/bin/app"), std::string::npos) << msg;
    EXPECT_NE(msg.find("built for 'aarch64-linux-musl'"), std::string::npos) << msg;
    EXPECT_NE(msg.find("[target.aarch64-linux-musl]"), std::string::npos) << msg;
    EXPECT_NE(msg.find("runner = [\"qemu-aarch64-static\"]"), std::string::npos) << msg;
    EXPECT_NE(msg.find("--no-runner"), std::string::npos) << msg;
}

TEST(RunnerLookup, SpawnFailedMessageIsVerbatim) {
    auto msg = spawn_failed_message("/x/bin/qemu", EACCES);
    EXPECT_NE(msg.find("'/x/bin/qemu' could not be started"), std::string::npos) << msg;
    EXPECT_NE(msg.find("Permission denied"), std::string::npos) << msg;
    EXPECT_NE(msg.find("(error 13)"), std::string::npos) << msg;
    EXPECT_EQ(msg.find("runner = ["), std::string::npos) << msg;   // no runner advice
}

#else

TEST(RunnerLookup, WindowsCoveredByIntegration) { SUCCEED(); }

#endif
