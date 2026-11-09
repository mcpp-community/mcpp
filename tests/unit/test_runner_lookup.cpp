#include <cerrno>
#include <fstream>

import std;
import mcpplibs.mtest;
import mcpp.build.runner_lookup;

using namespace mcpplibs::mtest;

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

UTest payload_bin_beats_path{"RunnerLookup/PayloadBinBeatsPath", [] {
    auto root = fresh_root("mcpp-runner-lookup-1");
    auto inPayload = make_exe(root / "payload" / "bin", "qemu-x");
    make_exe(root / "path", "qemu-x");
    std::vector<std::filesystem::path> bins{root / "payload" / "bin"};
    auto l = locate("qemu-x", bins, (root / "path").string());
    expect(fatal(l.program.has_value()));
    expect(eq(*l.program, inPayload));
}};

UTest path_is_searched_after_payloads{"RunnerLookup/PathIsSearchedAfterPayloads", [] {
    auto root = fresh_root("mcpp-runner-lookup-2");
    auto onPath = make_exe(root / "path", "qemu-y");
    std::vector<std::filesystem::path> bins{root / "payload" / "bin"};   // absent dir
    auto l = locate("qemu-y", bins, (root / "path").string());
    expect(fatal(l.program.has_value()));
    expect(eq(*l.program, onPath));
    expect(fatal(eq(l.searched.size(), 2u)));
    expect(eq(l.searched[0], root / "payload" / "bin"));
    expect(eq(l.searched[1], root / "path"));
}};

UTest non_executable_file_is_skipped{"RunnerLookup/NonExecutableFileIsSkipped", [] {
    auto root = fresh_root("mcpp-runner-lookup-3");
    std::filesystem::create_directories(root / "p1");
    { std::ofstream o(root / "p1" / "tool"); o << "data"; }   // no exec bit
    auto real = make_exe(root / "p2", "tool");
    auto l = locate("tool", {}, (root / "p1").string() + ":" + (root / "p2").string());
    expect(fatal(l.program.has_value()));
    expect(eq(*l.program, real));
}};

UTest not_found_lists_every_directory_searched{"RunnerLookup/NotFoundListsEveryDirectorySearched", [] {
    auto root = fresh_root("mcpp-runner-lookup-4");
    std::vector<std::filesystem::path> bins{root / "a" / "bin"};
    auto l = locate("nope", bins, (root / "p1").string() + ":" + (root / "p2").string());
    expect(!l.program.has_value());
    expect(fatal(eq(l.searched.size(), 3u)));
    auto msg = not_found_message("aarch64-linux-musl", "nope", l.searched);
    expect(ne(msg.find("runner 'nope'"), std::string::npos)) << msg;
    expect(ne(msg.find("aarch64-linux-musl"), std::string::npos)) << msg;
    expect(ne(msg.find((root / "a" / "bin").string()), std::string::npos)) << msg;
    expect(ne(msg.find((root / "p2").string()), std::string::npos)) << msg;
    expect(ne(msg.find("[xlings.workspace]"), std::string::npos)) << msg;
    expect(ne(msg.find("--no-runner"), std::string::npos)) << msg;
}};

UTest absolute_argv0_is_taken_as_is{"RunnerLookup/AbsoluteArgv0IsTakenAsIs", [] {
    auto root = fresh_root("mcpp-runner-lookup-5");
    auto abs = make_exe(root, "runner.sh");
    auto l = locate(abs.string(), {}, "");
    expect(fatal(l.program.has_value()));
    expect(eq(*l.program, abs));
    // ...and an absolute path that is not there is not searched for elsewhere.
    auto missing = locate((root / "absent.sh").string(), {}, root.string());
    expect(!missing.program.has_value());
}};

UTest empty_path_entries_are_ignored{"RunnerLookup/EmptyPathEntriesAreIgnored", [] {
    auto root = fresh_root("mcpp-runner-lookup-6");
    auto onPath = make_exe(root / "p", "tool");
    auto l = locate("tool", {}, ":" + (root / "p").string() + "::");
    expect(fatal(l.program.has_value()));
    expect(eq(*l.program, onPath));
    expect(eq(l.searched.size(), 1u));
}};

UTest classifies_enoexec_as_unloadable{"RunnerLookup/ClassifiesENOEXECAsUnloadable", [] {
    expect(eq(classify(ENOEXEC), SpawnClass::Unloadable));
    expect(eq(classify(EACCES), SpawnClass::Other));
    expect(eq(classify(ENOENT), SpawnClass::Other));
    expect(eq(classify(0), SpawnClass::Other));
}};

UTest unrunnable_message_names_kernel_answer_triple_and_key{"RunnerLookup/UnrunnableMessageNamesKernelAnswerTripleAndKey", [] {
    auto msg = unrunnable_message("aarch64-linux-musl", "/x/bin/app", ENOEXEC);
    expect(ne(msg.find("Exec format error"), std::string::npos)) << msg;
    expect(ne(msg.find("/x/bin/app"), std::string::npos)) << msg;
    expect(ne(msg.find("built for 'aarch64-linux-musl'"), std::string::npos)) << msg;
    expect(ne(msg.find("[target.aarch64-linux-musl]"), std::string::npos)) << msg;
    expect(ne(msg.find("runner = [\"qemu-aarch64-static\"]"), std::string::npos)) << msg;
    expect(ne(msg.find("--no-runner"), std::string::npos)) << msg;
}};

// A PAYLOAD CONTRIBUTES TWO DIRECTORIES, AND THE ORDER IS PART OF THE RULE.
// The flat case is the one measured missing on a macOS runner: a program at
// the payload root with no `bin/`, reached through exactly the directories
// prepare collects for each installed payload.
UTest a_flat_payload_is_found_at_its_root{"RunnerLookup/AFlatPayloadIsFoundAtItsRoot", [] {
    auto root = fresh_root("mcpp-runner-lookup-flat");
    auto flat = make_exe(root / "payload", "simctl-run");
    auto l = locate("simctl-run", payload_search_dirs(root / "payload"), "");
    expect(fatal(l.program.has_value()));
    expect(eq(*l.program, flat));
    expect(fatal(eq(l.searched.size(), 2u)));
    expect(eq(l.searched[0], root / "payload" / "bin"));
    expect(eq(l.searched[1], root / "payload"));
}};

// And a recipe that follows the convention is unaffected by the second entry.
UTest a_payloads_bin_is_tried_before_its_root{"RunnerLookup/APayloadsBinIsTriedBeforeItsRoot", [] {
    auto root = fresh_root("mcpp-runner-lookup-binfirst");
    auto inBin = make_exe(root / "payload" / "bin", "tool");
    make_exe(root / "payload", "tool");
    auto l = locate("tool", payload_search_dirs(root / "payload"), "");
    expect(fatal(l.program.has_value()));
    expect(eq(*l.program, inBin));
}};

UTest spawn_failed_message_is_verbatim{"RunnerLookup/SpawnFailedMessageIsVerbatim", [] {
    auto msg = spawn_failed_message("/x/bin/qemu", EACCES);
    expect(ne(msg.find("'/x/bin/qemu' could not be started"), std::string::npos)) << msg;
    expect(ne(msg.find("Permission denied"), std::string::npos)) << msg;
    expect(ne(msg.find("(error 13)"), std::string::npos)) << msg;
    expect(eq(msg.find("runner = ["), std::string::npos)) << msg;   // no runner advice
}};

#else

UTest runner_lookup_windows_covered_by_integration{"RunnerLookup/WindowsCoveredByIntegration", [] { /* passes */; }};

#endif
