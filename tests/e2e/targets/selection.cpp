// The compiler a row is built with, and what the report says of it: what the report names is what
// the link line uses, a capability pin is not a preference, a convention is overridden only by a
// replacement, the graph's compiler is taken without writing anything, a stated compiler outranks
// the graph and two requirements do not stack, the query gives one answer for the C library, and a
// capability pin explains its own row (formerly tests/e2e/296, 297, 298, 301, 302, 303, 640).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

// jq's `<path> // "<fallback>"`: the value at `path`, the fallback when it is absent, null or false.
std::string jstr(const nlohmann::json& j, std::initializer_list<std::string_view> path, std::string_view fallback = "") {
    const nlohmann::json* at = &j;
    for (auto k : path) {
        if (!at->is_object() || !at->contains(k)) return std::string(fallback);
        at = &(*at)[std::string(k)];
    }
    if (at->is_null() || (at->is_boolean() && !at->get<bool>())) return std::string(fallback);
    return at->is_string() ? at->get<std::string>() : at->dump();
}

// jq's `.diagnostics[].message`, one per line.
std::string messages(const nlohmann::json& j) {
    std::string out;
    if (j.is_object() && j.contains("diagnostics") && j["diagnostics"].is_array())
        for (const auto& d : j["diagnostics"])
            if (d.contains("message") && d["message"].is_string()) out += d["message"].get<std::string>() + "\n";
    return out;
}

// jq's `[.data.layers[] | select(.layer == "<layer>") | .origin][0] // "<fallback>"`.
std::string layer_origin(const nlohmann::json& j, std::string_view layer, std::string_view fallback) {
    if (j.is_object() && j.contains("data") && j["data"].contains("layers") && j["data"]["layers"].is_array())
        for (const auto& l : j["data"]["layers"])
            if (l.value("layer", std::string()) == layer) return jstr(l, {"origin"}, fallback);
    return std::string(fallback);
}

std::vector<std::string> lines_of(std::string_view text) {
    std::vector<std::string> out;
    for (auto part : std::views::split(text, '\n')) {
        std::string line(part.begin(), part.end());
        if (line.ends_with('\r')) line.pop_back();
        out.push_back(std::move(line));
    }
    return out;
}

// The first line of the build's build.ninja (not a build program's) starting with `channel`.
std::string ninja_line(const fs::path& target, std::string_view channel) {
    for (const auto& f : s::find_files(target, R"(build\.ninja)")) {
        if (f.generic_string().contains("/.build-mcpp/")) continue;
        for (const auto& l : lines_of(s::read_file(f)))
            if (l.starts_with(channel)) return l;
        return {};
    }
    return {};
}

nlohmann::json toolchain_list(const fs::path& cwd) {
    return s::json(s::mcpp({"toolchain", "list", "--format", "json"}, cwd).out);
}

// `[.data.toolchains[] | select(.family == "<family>") | .version][0] // empty`.
std::string installed_version(const nlohmann::json& list, std::string_view family) {
    if (list.is_object() && list.contains("data") && list["data"].contains("toolchains"))
        for (const auto& t : list["data"]["toolchains"])
            if (t.value("family", std::string()) == family) return t.value("version", std::string());
    return {};
}

std::string uname_m() {
    if (s::has("x86_64-host")) return "x86_64";
    return s::on_macos() ? "arm64" : "aarch64";
}

// Every string a key "target" holds anywhere in the document.
void targets_in(const nlohmann::json& j, std::vector<std::string>& out) {
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (it.key() == "target" && it.value().is_string()) out.push_back(it.value().get<std::string>());
            targets_in(it.value(), out);
        }
    } else if (j.is_array()) {
        for (const auto& e : j) targets_in(e, out);
    }
}

E2E selection{"selection", [] {
    "296 what the report names is what the link line uses"_test[tag{"requires:gcc"}][tag{"requires:unix-shell"}][tag{"requires:jq"}] = [] {
        // Every toolchain the list names round-trips through --toolchain; the C library the query
        // names is on the link line (or, when it names none, the link line reaches nothing
        // outside the store); a graph C library does not drag this host's in.
        s::needs({"gcc", "unix-shell", "jq"});
        s::project p({{"src/main.cpp", "#include <cstdio>\nint main() { std::printf(\"ok\\n\"); }\n"}});
        std::string report, ldflags;
        auto build_and_read = [&](std::vector<std::string> extra) {
            p.remove("target");
            std::vector<std::string> args{"build"};
            args.insert(args.end(), extra.begin(), extra.end());
            report = s::text(p.mcpp(args));
            ldflags = ninja_line(p.at("target"), "ldflags");
        };
        auto c_abi_line = [&] {
            static const std::regex re{R"(^\s+c-abi\s)"};
            for (const auto& l : lines_of(report))
                if (std::regex_search(l, re)) return l;
            return std::string();
        };
        int checked = 0;
        const auto list = toolchain_list(p.dir());
        if (list.is_object() && list.contains("data") && list["data"].contains("toolchains"))
            for (const auto& t : list["data"]["toolchains"]) {
                const auto spec = t.value("family", std::string()) + "@" + t.value("version", std::string());
                const bool round_trips = static_cast<bool>(p.mcpp({"why", "toolchain", "--toolchain", spec, "--format", "json"}));
                expect(round_trips) << "FAIL: toolchain list emitted '" << spec << "' and --toolchain refuses it";
                if (round_trips) ++checked;
            }
        for (std::string tc : {"gcc", "llvm"}) {
            const auto ver = installed_version(list, tc);
            if (ver.empty()) {
                std::println("  SKIP  {} is not installed here", tc);
                continue;
            }
            p.write("mcpp.toml", std::format("[package]\nname    = \"linkprobe\"\nversion = \"0.1.0\"\n\n[toolchain]\ndefault = \"{}@{}\"\n", tc, ver));
            build_and_read({"--verbose"});
            const auto line = c_abi_line();
            if (!line.contains("(payload)")) {
                std::println("  SKIP  {}@{}: c-abi is not the payload's here ({})", tc, ver, line.empty() ? "no row" : line);
                continue;
            }
            if (ldflags.empty()) {
                std::println("  SKIP  {}@{} produced no link line", tc, ver);
                continue;
            }
            ++checked;
            const auto want = jstr(s::json(p.mcpp({"why", "toolchain", "--toolchain", tc + "@" + ver, "--format", "json"}).out), {"data", "cLibrary", "path"});
            if (want.empty()) {
                // A flag and its separated value are one word; the words that name an absolute
                // directory outside the store are a leak.
                const auto joined = std::regex_replace(ldflags, std::regex{R"((--sysroot|-isysroot|-B|-L)[ \t]+)"}, "$1");
                static const std::regex path_word{R"(^(--sysroot=?|-isysroot=?|-B|-L)[^ ]*[/\\][^ ]*$)"};
                std::vector<std::string> leak;
                for (auto part : std::views::split(std::string_view(joined), ' ')) {
                    std::string w(part.begin(), part.end());
                    if (std::regex_search(w, path_word) && !w.contains("registry")) leak.push_back(w);
                }
                expect(leak.empty()) << "FAIL: " << tc << "@" << ver << ": the query names no C library, and the link line reaches outside: " << nlohmann::json(leak).dump();
                continue;
            }
            expect(e2e::contains(ldflags, want)) << "FAIL: " << tc << "@" << ver << ": the query names a C library the link line does not use\n  query:   " << want << "\n  ldflags: " << ldflags.substr(0, 110);
        }
        p.write("mcpp.toml", std::format("[package]\nname    = \"linkprobe\"\nversion = \"0.1.0\"\n\n[toolchain]\ndefault = \"llvm@{}\"\n\n[dependencies]\nopenkal-musl = \"0.3.5\"\nopenkal-llvm-runtime = \"0.1.3\"\n", s::llvm_version()));
        p.write("src/main.cpp", "import std;\nint main(){ std::println(\"ok\"); }\n");
        build_and_read({});
        const auto line = c_abi_line();
        if (line.contains("graph")) {
            if (ldflags.empty()) {
                std::println("  SKIP  the graph build produced no link line");
            } else {
                ++checked;
                expect(s::no_line(ldflags, "xim-x-glibc|/lib/x86_64-linux-gnu|/usr/lib/gcc")) << "FAIL: c-abi comes from the graph, yet the link line reaches for this host's C library";
            }
        } else {
            std::println("  SKIP  the graph did not supply the C library here ({})", line.empty() ? "no row" : line);
        }
        if (checked == 0) skip("nothing here produced a link line to relate to a report");
    };

    "297 a capability pin is not a preference"_test[tag{"requires:gcc"}][tag{"requires:unix-shell"}][tag{"requires:jq"}] = [] {
        // A bare-metal row refuses a declared compiler that cannot emit it, naming the target,
        // the reason and the way out; a hosted row still honours the declared toolchain.
        s::needs({"gcc", "unix-shell", "jq"});
        const auto tmp = s::case_dir();
        s::project p(tmp / "work", {{"src/main.cpp", "extern \"C\" int main(int, char**, char**) { return 0; }\n"}});
        const auto gccver = installed_version(toolchain_list(p.dir()), "gcc");
        if (gccver.empty()) skip("gcc is not installed here, and this test is about declaring it");
        s::project probe(tmp / "probe", {{"mcpp.toml", "[package]\nname = \"gate\"\nversion = \"0.1.0\"\n"},
                                         {"src/main.cpp", "int main() { return 0; }\n"}});
        const auto serve = jstr(s::json(probe.mcpp({"why", "toolchain", "--toolchain", "gcc@" + gccver, "--target", uname_m() + "-linux-gnu", "--format", "json"}).out), {"data", "reason"}, "none");
        if (serve != "none") skip("gcc is not installed here, and this test is about declaring it");
        p.write("mcpp.toml", std::format("[package]\nname    = \"capprobe\"\nversion = \"0.1.0\"\n\n[toolchain]\ndefault = \"gcc@{}\"\n\n[target.riscv64-none-elf]\nsysroot = \"\"\n", gccver));
        p.remove("target");
        const auto out = s::text(p.mcpp({"build", "--target", "riscv64-none-elf"}));
        const auto reason = jstr(s::json(p.mcpp({"why", "toolchain", "--target", "riscv64-none-elf", "--toolchain", "gcc@" + gccver, "--format", "json"}).out), {"data", "reason"}, "-");
        expect(fatal(reason != "none")) << "FAIL: the declaration was honoured and the build was left to fail later: " << out;
        if (reason != "capability-pin") {
            expect(fatal(s::lacks(out, "Finished"))) << "FAIL: gcc built a bare-metal riscv64 target — that should not be possible";
            skip(std::format("the refusal reason here was '{}', not one this test knows", reason));
        }
        expect(fatal(e2e::contains(out, "riscv64-none-elf") && s::contains_nocase(out, "capability") && e2e::contains(out, "[toolchain]"))) << "FAIL: the refusal does not point at the decision: " << out;
        p.write("mcpp.toml", std::format("[package]\nname    = \"capprobe\"\nversion = \"0.1.0\"\n\n[toolchain]\ndefault = \"gcc@{}\"\n", gccver));
        p.write("src/main.cpp", "#include <cstdio>\nint main() { std::printf(\"ok\\n\"); }\n");
        const auto hosted_target = jstr(s::json(p.mcpp({"why", "toolchain", "--toolchain", "gcc@" + gccver, "--format", "json"}).out), {"data", "triple", "toolchain"});
        if (hosted_target.empty()) skip(std::format("the query did not name a target for gcc@{} on this host", gccver));
        p.remove("target");
        const auto hosted = s::text(p.mcpp({"build", "--target", hosted_target}));
        auto hosted_reason = jstr(s::json(p.mcpp({"why", "toolchain", "--target", hosted_target, "--toolchain", "gcc@" + gccver, "--format", "json"}).out), {"data", "reason"}, "-");
        if (hosted_reason.empty()) hosted_reason = "-";
        expect(fatal(hosted_reason != "capability-pin")) << "FAIL: " << hosted_target << " refused a declared toolchain — the escape hatch is gone: " << hosted;
        if (hosted_reason != "none") skip(std::format("the hosted control's reason was '{}', not one this test knows", hosted_reason));
        expect(fatal(e2e::contains(hosted, "Finished"))) << "FAIL: the hosted control resolved but did not build: " << hosted;
    };

    "298 overriding a convention requires replacing it"_test[tag{"requires:llvm"}][tag{"requires:unix-shell"}][tag{"requires:jq"}] = [] {
        // A row served by a gcc convention refuses a declared llvm with nothing in the
        // convention's place, naming the target, the convention and the replacement; the same
        // declaration is honoured when the graph supplies the C library.
        s::needs({"llvm", "unix-shell", "jq"});
        const auto tmp = s::case_dir();
        s::project p(tmp / "work", {{"src/main.cpp", "#include <cstdio>\nint main() { std::printf(\"ok\\n\"); }\n"}});
        const auto list = toolchain_list(p.dir());
        const auto llvmver = installed_version(list, "llvm");
        if (llvmver.empty()) skip("llvm is not installed here, and this test is about declaring it");
        const auto host = jstr(list, {"data", "host"});
        const auto host_arch = host.substr(0, host.find('-'));
        s::project neutral(tmp / "neutral", {{"mcpp.toml", "[package]\nname    = \"neutral\"\nversion = \"0.1.0\"\n"}});
        auto served_by_its_convention = [&](const std::string& t) {
            const auto fam = jstr(s::json(neutral.mcpp({"why", "toolchain", "--target", t, "--format", "json"}).out), {"data", "compiler", "family"});
            return !fam.empty() && fam != "llvm" && fam != "clang";
        };
        auto pick = [&](const std::string& arch) -> std::string {
            std::set<std::string> rows;
            if (list.is_object() && list.contains("data") && list["data"].contains("targets"))
                for (const auto& t : list["data"]["targets"]) {
                    const auto pin = jstr(t, {"pin"}), target = jstr(t, {"target"}), status = jstr(t, {"status"});
                    if (!pin.starts_with("gcc") || target == "x86_64-windows-gnu" || status == "planned") continue;
                    if (!arch.empty() && !target.starts_with(arch + "-")) continue;
                    rows.insert(target);
                }
            for (const auto& t : rows)
                if (served_by_its_convention(t)) return t;
            return {};
        };
        auto pinned = pick(host_arch);
        if (pinned.empty()) pinned = pick("");
        if (pinned.empty()) skip("no target row here is served by a non-llvm convention");
        p.write("mcpp.toml", std::format("[package]\nname    = \"convprobe\"\nversion = \"0.1.0\"\n\n[toolchain]\ndefault = \"llvm@{}\"\n", llvmver));
        p.remove("target");
        const auto out = s::text(p.mcpp({"build", "--target", pinned}));
        auto why = [&] { return s::json(p.mcpp({"why", "toolchain", "--target", pinned, "--toolchain", "llvm@" + llvmver, "--format", "json"}).out); };
        const auto reason = jstr(why(), {"data", "reason"}, "-");
        if (reason == "none") {
            expect(false) << (e2e::contains(out, "Finished") ? "FAIL: " + pinned + " linked with llvm alone — the refusal has outlived its reason"
                                                             : "FAIL: the build ran and failed at the link instead of at the decision: " + out);
        } else if (reason != "convention-unreplaced") {
            skip(std::format("{} refused under '{}', which is not the rule this test is about", pinned, reason));
        } else {
            expect(e2e::contains(out, pinned) && e2e::contains(out, "openkal") && e2e::contains(out, "[toolchain]")) << "FAIL: the refusal does not point at the way out: " << out;
        }
        p.write("mcpp.toml", std::format("[package]\nname    = \"convprobe\"\nversion = \"0.1.0\"\n\n[dependencies]\nopenkal-llvm-runtime = \"0.1.3\"\n\n[toolchain]\ndefault = \"llvm@{}\"\n", llvmver));
        p.write("src/main.cpp", "import std;\nint main() { std::println(\"ok\"); }\n");
        p.remove("target");
        const auto graph = s::text(p.mcpp({"build", "--target", pinned, "--verbose"}));
        const auto doc = why();
        const auto graph_reason = jstr(doc, {"data", "reason"}, "-");
        const auto graph_cabi = layer_origin(doc, "c-abi", "-");
        expect(graph_reason != "convention-unreplaced") << "FAIL: a graph-supplied C library was refused — the escape hatch is gone: " << graph;
        if (graph_reason != "convention-unreplaced")
            expect(graph_cabi == "graph") << "FAIL: c-abi came from '" << graph_cabi << "', not the graph — half two proves nothing: " << graph;
    };

    "301 the graphs compiler is taken and nothing is written"_test[tag{"requires:unix-shell"}][tag{"requires:jq"}] = [] {
        // A dependency's `requires = ["mcpp:compiler=<family>"]` selects that compiler where mcpp's
        // own answer was another, says who asked and what it displaced, and leaves the machine's
        // configuration byte-identical.
        s::needs({"unix-shell", "jq"});
        const auto tmp = s::case_dir();
        s::project app(tmp / "app", {{"src/main.cpp", "extern \"C\" int main(int, char**, char**) { return 0; }\n"},
                                     {"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n"}});
        s::project needs(tmp / "needs", {{"src/needs_it.cppm", "export module needs_it;\nexport int needs_it() { return 0; }\n"}});
        std::set<std::string> installed;
        const auto list = toolchain_list(app.dir());
        if (list.is_object() && list.contains("data") && list["data"].contains("toolchains"))
            for (const auto& t : list["data"]["toolchains"]) installed.insert(t.value("family", std::string()));
        auto current = jstr(s::json(app.mcpp({"why", "toolchain", "--format", "json"}).out), {"data", "compiler", "family"}, "-");
        if (current == "clang") current = "llvm";
        std::string want;
        for (const auto& f : installed) {
            if (f == current) continue;
            if (f == "gcc" || f == "llvm") { want = f; break; }
        }
        if (want.empty()) skip(std::format("this project already resolves '{}' and no other family is installed here, so no requirement could differ from it", current));
        needs.write("mcpp.toml", std::format("[package]\nname     = \"needs-{0}\"\nversion  = \"0.1.0\"\nrequires = [\"mcpp:compiler={0}\"]\n", want));
        app.write("mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\nneeds-{} = {{ path = \"../needs\" }}\n", want));
        const auto cfg = s::shared_home() / "config.toml";
        std::string before;
        if (fs::is_regular_file(cfg)) {
            before = s::sha256(s::read_file(cfg));
            expect(fatal(!before.empty())) << "FAIL: could not hash " << cfg.string();
        }
        const auto j = s::json(app.mcpp({"why", "toolchain", "--format", "json"}).out);
        const auto reason = jstr(j, {"data", "reason"}, "-");
        auto got = jstr(j, {"data", "compiler", "family"}, "-");
        if (got == "clang") got = "llvm";
        expect(fatal(reason == "none")) << "FAIL: the graph required '" << want << "' and mcpp refused with '" << reason << "'\n" << messages(j);
        expect(fatal(got == want)) << "FAIL: the graph required '" << want << "', mcpp resolved '" << got << "'";
        std::string after;
        if (fs::is_regular_file(cfg)) after = s::sha256(s::read_file(cfg));
        expect(fatal(!before.empty() && !after.empty())) << "FAIL: one side of the comparison is empty (before='" << before << "' after='" << after << "'), so nothing was actually compared";
        expect(fatal(before == after)) << "FAIL: selecting the graph's compiler rewrote the global configuration\n  before " << before << "\n  after  " << after;
        const auto by = jstr(j, {"data", "compiler", "chosenBy", "requiredBy"});
        const auto was = jstr(j, {"data", "compiler", "chosenBy", "replaced"});
        expect(fatal(!by.empty())) << "FAIL: compiler.chosenBy.requiredBy is empty — the document does not say a dependency decided this";
        expect(fatal(by.starts_with("needs-" + want + "@"))) << "FAIL: compiler.chosenBy.requiredBy is '" << by << "', expected needs-" << want << "@…";
        expect(fatal(!was.empty())) << "FAIL: compiler.chosenBy.replaced is empty, yet '" << current << "' was displaced";
        const auto out = s::text(app.mcpp({"build"}));
        expect(fatal(e2e::contains(out, "needs-" + want) && e2e::contains(out, "mcpp:compiler=" + want))) << "FAIL: the resolution does not say who required this compiler: " << out;
    };

    "302 a stated compiler outranks the graph and two requirements do not stack"_test[tag{"requires:unix-shell"}][tag{"requires:jq"}] = [] {
        // A compiler the project states is not revised by the graph (and the remedy names where it
        // was stated); two packages requiring different compilers are an error naming both; a
        // capability row refuses a requirement it cannot satisfy and offers no remedy it ignores.
        // The rule is decided in the build's preparation (its toolchain decision), which exports
        // no function that answers it.
        s::needs({"unix-shell", "jq"});
        const auto tmp = s::case_dir();
        std::set<std::string> installed;
        const auto list = toolchain_list(tmp);
        if (list.is_object() && list.contains("data") && list["data"].contains("toolchains"))
            for (const auto& t : list["data"]["toolchains"]) {
                const auto family = t.value("family", std::string());
                if (family == "gcc" || family == "llvm") installed.insert(family + "@" + t.value("version", std::string()));
            }
        std::string gccspec, llvmspec;
        for (const auto& spec : installed) {
            if (gccspec.empty() && spec.starts_with("gcc@")) gccspec = spec;
            if (llvmspec.empty() && spec.starts_with("llvm@")) llvmspec = spec;
        }
        s::project(tmp / "needs-gcc", {{"mcpp.toml", "[package]\nname     = \"needs-gcc\"\nversion  = \"0.1.0\"\nrequires = [\"mcpp:compiler=gcc\"]\n"},
                                       {"src/a.cppm", "export module needs_gcc;\nexport int ng() { return 1; }\n"}});
        s::project(tmp / "needs-llvm", {{"mcpp.toml", "[package]\nname     = \"needs-llvm\"\nversion  = \"0.1.0\"\nrequires = [\"mcpp:compiler=llvm\"]\n"},
                                        {"src/b.cppm", "export module needs_llvm;\nexport int nl() { return 2; }\n"}});
        s::project app(tmp / "app", {{"src/main.cpp", "extern \"C\" int main(int, char**, char**) { return 0; }\n"}});
        auto why = [&](std::vector<std::string> extra) {
            std::vector<std::string> args{"why", "toolchain"};
            args.insert(args.end(), extra.begin(), extra.end());
            args.insert(args.end(), {"--format", "json"});
            return s::json(app.mcpp(args).out);
        };
        if (!gccspec.empty() && !llvmspec.empty()) {
            app.write("mcpp.toml", std::format("[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[toolchain]\ndefault = \"{}\"\n\n[dependencies]\nneeds-llvm = {{ path = \"../needs-llvm\" }}\n", gccspec));
            const auto j = why({});
            const auto reason = jstr(j, {"data", "reason"}, "-");
            const auto msg = messages(j);
            expect(fatal(reason == "layer-requirement")) << "FAIL: a stated compiler was overridden by the graph (reason '" << reason << "')\n" << msg;
            expect(fatal(e2e::contains(msg, "[toolchain]") && e2e::contains(msg, "mcpp.toml"))) << "FAIL: the refusal does not name where the compiler was stated\n" << msg;
            expect(fatal(s::lacks(msg, "mcpp toolchain default"))) << "FAIL: the remedy is a global change that would not fix this failure";
        } else {
            std::println("SKIP(half one): both gcc and llvm must be installed to state one of them");
        }
        app.write("mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\nneeds-gcc  = { path = \"../needs-gcc\" }\nneeds-llvm = { path = \"../needs-llvm\" }\n");
        const auto j2 = why({});
        const auto reason2 = jstr(j2, {"data", "reason"}, "-");
        const auto msg2 = messages(j2);
        expect(fatal(reason2 == "compiler-requirement-conflict")) << "FAIL: two conflicting requirements gave reason '" << reason2 << "'\n" << msg2;
        expect(fatal(e2e::contains(msg2, "needs-gcc") && e2e::contains(msg2, "needs-llvm"))) << "FAIL: the conflict does not name both packages\n" << msg2;
        app.write("mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\nneeds-gcc = { path = \"../needs-gcc\" }\n");
        const auto j3 = why({"--target", "riscv64-none-elf"});
        const auto reason3 = jstr(j3, {"data", "reason"}, "-");
        const auto msg3 = messages(j3);
        expect(fatal(reason3 == "compiler-requirement-conflict")) << "FAIL: a capability row against a gcc requirement gave reason '" << reason3 << "'\n" << msg3;
        expect(fatal(s::contains_nocase(msg3, "capability"))) << "FAIL: the refusal does not say the row's pin is a capability\n" << msg3;
        expect(fatal(s::lacks(msg3, "supplies this target's system"))) << "FAIL: the remedy offers to supply the system, which a capability pin ignores";
    };

    "303 the query gives one answer for the c library"_test[tag{"requires:unix-shell"}][tag{"requires:jq"}] = [] {
        // `why toolchain --format json` says which of its two C-library answers governs
        // (`cLibrary.suppliesTarget`): with no dependency the two agree, and when the graph
        // supplies the c-abi the payload's model says it does not. The answer is computed inside
        // the query's command, which exports no function that answers it.
        s::needs({"unix-shell", "jq"});
        s::project p({{"src/main.cpp", "extern \"C\" int main(int, char**, char**) { return 0; }\n"},
                      {"mcpp.toml", "[package]\nname    = \"clibprobe\"\nversion = \"0.1.0\"\n"}});
        auto why = [&] { return s::json(p.mcpp({"why", "toolchain", "--format", "json"}).out); };
        auto supplies_of = [](const nlohmann::json& j) {
            if (!j.is_object() || !j.contains("data") || !j["data"].contains("cLibrary")) return std::string("null");
            const auto& c = j["data"]["cLibrary"];
            return c.contains("suppliesTarget") ? c["suppliesTarget"].dump() : std::string("null");
        };
        const auto base = why();
        const auto basereason = jstr(base, {"data", "reason"}, "QUERY-FAILED");
        expect(fatal(basereason == "none")) << "FAIL: the query refused ('" << basereason << "') on a dependency-free project\n" << messages(base);
        const bool has_supplies = base.contains("data") && base["data"].contains("cLibrary") && base["data"]["cLibrary"].is_object() && base["data"]["cLibrary"].contains("suppliesTarget");
        expect(fatal(has_supplies)) << "FAIL: cLibrary.suppliesTarget is absent — the document still cannot say which of its two C-library answers governs: " << (base.contains("data") && base["data"].contains("cLibrary") ? base["data"]["cLibrary"].dump() : std::string("null"));
        const auto probe = why();
        const auto origin = layer_origin(probe, "c-abi", "");
        const auto supplies = supplies_of(probe);
        const auto mode = jstr(base, {"data", "cLibrary", "mode"}, "null");
        expect(fatal(origin != "graph")) << "FAIL: a project with no dependencies took its c-abi from the graph";
        expect(fatal(!(supplies == "false" && mode != "none"))) << "FAIL: the payload has a link model ('" << mode << "') and the c-abi is '" << origin << "', yet cLibrary says it does not supply the target";
        p.write("libc/mcpp.toml", "[package]\nname     = \"fake-libc\"\nversion  = \"0.1.0\"\nprovides = [\"mcpp:c-abi=musl\"]\n");
        p.write("libc/src/fake_libc.cppm", "export module fake_libc;\nexport int fl() { return 0; }\n");
        p.write("mcpp.toml", "[package]\nname    = \"clibprobe\"\nversion = \"0.1.0\"\n\n[dependencies]\nfake-libc = { path = \"libc\" }\n");
        const auto doc2 = why();
        const auto reason2 = jstr(doc2, {"data", "reason"}, "QUERY-FAILED");
        if (reason2 != "none") skip(std::format("this host refuses a musl c-abi over its own target ('{}'), so there is no two-answer document to check here", reason2));
        const auto probe2 = why();
        const auto origin2 = layer_origin(probe2, "c-abi", "");
        expect(fatal(origin2 == "graph")) << "FAIL: a package declaring `provides = [\"mcpp:c-abi=musl\"]` did not take the c-abi layer (origin '" << origin2 << "')";
        expect(fatal(supplies_of(probe2) == "false")) << "FAIL: c-abi comes from the graph, yet cLibrary claims to supply the target: " << (probe2.contains("data") ? probe2["data"].dump() : std::string());
    };

    "640 a capability pin explains its own row"_test[tag{"requires:gcc"}] = [] {
        // Every capability-pinned row refuses a compiler that cannot emit it with the same rule
        // and its own reason; the PE+musl sentence belongs to exactly one row.
        s::needs({"gcc"});
        const auto tmp = s::case_dir();
        int n = 0;
        auto build = [&](const std::string& target, const std::string& tc) {
            s::project p(tmp / std::format("p{}", n++), {{"mcpp.toml", std::format("[package]\nname = \"c\"\nversion = \"0.1.0\"\n\n[toolchain]\nlinux = \"{}\"\n", tc)},
                                                         {"src/main.cpp", "int main(){return 0;}\n"}});
            return s::text(p.mcpp({"build", "--target", target}, {.env = {{"MCPP_NO_AUTO_INSTALL", "1"}}}));
        };
        auto check = [&](const std::string& target, const std::string& tc, std::string_view phrase, std::string_view label) {
            const auto out = build(target, tc);
            if (!e2e::contains(out, "cannot be emitted by")) {
                expect(false) << "FAIL: " << label << " — not refused at all: " << out;
                return;
            }
            expect(e2e::contains(out, phrase)) << "FAIL: " << label << " — the refusal does not carry this row's reason; wanted: " << phrase << "\n" << out;
        };
        const auto llvm = "llvm@" + s::llvm_version();
        check("wasm32-emscripten", "gcc@16.1.0", "Nothing but Emscripten emits WebAssembly", "wasm names Emscripten");
        check("riscv64-none-elf", "gcc@16.1.0", "no per-host cross payload", "bare metal names the cross payload");
        check("x86_64-windows-musl", "gcc@16.1.0", "PE with a musl C library", "PE+musl names the C library");
        check("aarch64-linux-android", "gcc@16.1.0", "An Android target needs bionic", "android names bionic");
        check("x86_64-linux-android", "gcc@16.1.0", "An Android target needs bionic", "android names bionic (x86_64)");
        check("wasm32-emscripten", llvm, "Nothing but Emscripten emits WebAssembly", "a declared llvm is refused too");
        check("aarch64-linux-android", llvm, "An Android target needs bionic", "a declared llvm is refused for android too");
        const auto pin = build("wasm32-emscripten", "gcc@16.1.0");
        expect(s::has_line(pin, "names .emsdk@6.0.9. as a capability")) << "FAIL: the closing line does not name emsdk@6.0.9";
        // The PE+musl sentence belongs to exactly one row.
        std::vector<std::string> pinned;
        targets_in(toolchain_list(tmp), pinned);
        expect(fatal(!pinned.empty())) << "FAIL: toolchain list named no targets";
        int pe_musl = 0, examined = 0;
        for (const auto& target : pinned) {
            const auto out = build(target, "gcc@16.1.0");
            if (!e2e::contains(out, "cannot be emitted by")) continue;  // not a capability row
            ++examined;
            if (e2e::contains(out, "PE with a musl C library")) {
                ++pe_musl;
                expect(target == "x86_64-windows-musl") << "FAIL: " << target << " is explained with the PE+musl sentence";
            }
        }
        std::println("  examined {} capability-pinned rows of {} targets", examined, pinned.size());
        expect(examined >= 4) << "FAIL: only " << examined << " capability rows were reached — the enumeration is too small to be evidence";
        expect(pe_musl == 1) << "FAIL: the PE+musl sentence was printed for " << pe_musl << " rows, expected exactly 1";
    };
}};

}  // namespace
