// What a target request means: a triple states a request and the target side the fact, a build for
// one system is never performed for another, the list answers what can be built, naming the host's
// own target changes nothing, a request that names no C library reaches a row that exists, a
// registered family is not reported unknown, and `why flags` and `why target` name the layer of a
// word and the selection of a request (formerly tests/e2e/282, 293, 294, 295, 299, 300, 1042).
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

std::vector<std::string> lines_of(std::string_view text) {
    std::vector<std::string> out;
    for (auto part : std::views::split(text, '\n')) {
        std::string line(part.begin(), part.end());
        if (line.ends_with('\r')) line.pop_back();
        out.push_back(std::move(line));
    }
    return out;
}

// The first match of `pattern`'s first group in `text`, or "".
std::string first_group(const std::string& text, const std::regex& pattern) {
    std::smatch m;
    return std::regex_search(text, m, pattern) ? m[1].str() : std::string();
}

// The first build.ninja of the build (not a build program's), or nothing.
std::optional<fs::path> build_ninja(const fs::path& target) {
    for (const auto& f : s::find_files(target, R"(build\.ninja)"))
        if (!f.generic_string().contains("/.build-mcpp/")) return f;
    return std::nullopt;
}

// The version of the first installed toolchain of `family` (`toolchain list --format json`).
std::string installed_version(const fs::path& cwd, std::string_view family) {
    const auto list = s::json(s::mcpp({"toolchain", "list", "--format", "json"}, cwd).out);
    if (list.is_object() && list.contains("data") && list["data"].contains("toolchains"))
        for (const auto& t : list["data"]["toolchains"])
            if (t.value("family", std::string()) == family) return t.value("version", std::string());
    return {};
}

std::string uname_m() {
    if (s::has("x86_64-host")) return "x86_64";
    return s::on_macos() ? "arm64" : "aarch64";
}

E2E requests{"requests", [] {
    "282 target is a request"_test[tag{"requires:gcc"}] = [] {
        // A triple that declines to name a C library is no contradiction of the graph's; one
        // that names another is reported, with what resolved and a spelling to use instead.
        s::needs({"gcc"});
        const auto tmp = s::case_dir();
        s::project(tmp / "libc", {{"mcpp.toml", "[package]\nnamespace = \"probe\"\nname      = \"tiny-musl\"\nversion   = \"0.1.0\"\nprovides  = [\"mcpp:c-abi=musl\"]\n\n[build]\nsources = []\n"}});
        s::project app(tmp / "app", {{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n\n[dependencies]\ntiny-musl = { path = \"../libc\" }\n"},
                                     {"src/main.cpp", "int main(){ return 0; }\n"}});
        auto out = s::text(app.mcpp({"build", "--target", "x86_64-linux"}));
        expect(fatal(s::lacks(out, "requests the"))) << "declining to name a C library must not be a contradiction";
        expect(fatal(s::has_line(out, "Target x86_64-linux( |$)"))) << "the report must show the target as the project spelled it";
        expect(fatal(s::lacks(out, "Target x86_64-linux-gnu"))) << "a segment the project did not write must not appear in the report";
        out = s::text(app.mcpp({"build", "--target", "x86_64-linux-gnu"}));
        expect(fatal(s::has_line(out, "asks for the .gnu. C ABI"))) << "the mismatch was not reported";
        expect(fatal(e2e::contains(out, "musl"))) << "the report does not name what resolved: " << out;
        expect(fatal(e2e::contains(out, "--target x86_64-linux"))) << "the report names no correct spelling to use instead: " << out;
    };

    "293 the requested target and the resolved one name one os"_test[tag{"requires:gcc"}][tag{"requires:unix-shell"}] = [] {
        // A build for one operating system is never quietly performed for another, and a correct
        // cross build is never refused for it.
        s::needs({"gcc", "unix-shell"});
        const auto tmp = s::case_dir();
        auto make_project = [&](std::string_view dir, std::string_view section) {
            return s::project(tmp / dir, {{"mcpp.toml", std::format("[package]\nname    = \"osprobe\"\nversion = \"0.1.0\"\n{}", section.empty() ? std::string() : std::format("\n{}\n", section))},
                                          {"src/main.cpp", "#include <cstdio>\nint main() { std::printf(\"ok\\n\"); }\n"}});
        };
        static const std::regex target_line{R"(Target (\S+ → \S+))"};
        auto mismatch = make_project("mismatch", "[target.x86_64-windows-gnu]\ntoolchain = \"system\"");
        const auto out = s::text(mismatch.mcpp({"build", "--target", "x86_64-windows-gnu"}));
        const auto reported = first_group(out, target_line);
        bool half_one_done = false;
        if (e2e::contains(out, "different operating systems")) {
            std::println("  ok  it refuses rather than building for the wrong system");
        } else if (e2e::contains(out, "is not supported: mcpp builds only with toolchains it manages")) {
            std::println("  ok  the system toolchain is refused outright, so the mismatch this test guards cannot be reached through it");
            half_one_done = true;
        } else {
            const auto arrow = reported.find(" → ");
            const auto asked = reported.substr(0, arrow);
            const auto resolved = arrow == std::string::npos ? reported : reported.substr(arrow + std::string_view(" → ").size());
            expect(fatal(!(!reported.empty() && asked.contains("windows") && resolved.contains("linux")))) << "FAIL: a Windows target resolved to a Linux toolchain and the build went on: " << reported << "\n" << out;
            skip("this host did not reproduce the fallback");
        }
        if (!half_one_done) {
            expect(fatal(e2e::contains(out, "x86_64-windows-gnu") && e2e::contains(out, "linux"))) << "FAIL: the refusal does not name both systems: " << out;
        }
        int built = 0, skipped = 0;
        for (std::string t : {"x86_64-linux-gnu", "x86_64-linux-musl", "aarch64-linux-musl", "x86_64-windows-gnu"}) {
            auto p = make_project("ok-" + t, "");
            const auto o = s::text(p.mcpp({"build", "--target", t}));
            expect(fatal(s::lacks(o, "different operating systems"))) << "FAIL: a correct cross build was refused: " << t;
            if (e2e::contains(o, "Target ")) {
                std::println("  ok    {}", first_group(o, target_line));
                ++built;
            } else {
                ++skipped;
            }
        }
        if (built == 0) skip(std::format("no target resolved here — the sweep proved nothing ({} skipped)", skipped));
    };

    "294 the list answers what can be built not what has a payload"_test[tag{"requires:llvm"}][tag{"requires:unix-shell"}] = [] {
        // `toolchain list` names the targets this host can build for, the ones a dependency
        // graph serves included, and leaves out those that need a host-only toolchain.
        s::needs({"llvm", "unix-shell"});
        const auto out = s::text(s::mcpp({"toolchain", "list"}, s::case_dir()));
        std::vector<std::string> targets;
        bool in = false;
        for (auto& line : lines_of(out)) {
            if (!in && line.starts_with("Targets:")) in = true;
            if (in) targets.push_back(line);
        }
        if (targets.empty()) skip("this build printed no Targets section");
        auto row = [&](std::string_view t) -> std::string {
            const std::regex re{std::format(R"(^\s+\*?\s*{}(\s|$))", t)};
            for (const auto& l : targets)
                if (std::regex_search(l, re)) return l;
            return {};
        };
        if (!s::on_linux()) skip("the graph-served row under test is a Linux-host arrangement");
        std::string all;
        for (const auto& l : targets) all += l + "\n";
        const auto wm = row("x86_64-windows-musl");
        expect(fatal(!wm.empty())) << "FAIL: x86_64-windows-musl is absent, and this host can build it:\n" << all;
        expect(fatal(e2e::contains(wm, "dependency graph") || e2e::contains(wm, "installed") || e2e::contains(wm, "available"))) << "FAIL: listed with a status that says neither: " << wm;
        std::size_t n = 0;
        static const std::regex any_row{R"(^\s+\*?\s*[a-z0-9_]+-)"};
        for (const auto& l : targets)
            if (std::regex_search(l, any_row)) ++n;
        if (n < 3) skip(std::format("only {} target rows printed — too few to assert an absence", n));
        for (std::string_view t : {"x86_64-windows-msvc", "aarch64-macos"})
            expect(row(t).empty()) << "FAIL: " << t << " is offered on a Linux host, and no dependency supplies it: " << row(t);
    };

    "295 naming the host target changes nothing"_test[tag{"requires:gcc"}][tag{"requires:unix-shell"}][tag{"requires:jq"}] = [] {
        // `--target <the triple a toolchain uses anyway>` gives the same link and compile lines
        // as no --target, for each installed family.
        s::needs({"gcc", "unix-shell", "jq"});
        s::project p({{"src/main.cpp", "#include <cstdio>\nint main() { std::printf(\"ok\\n\"); }\n"}});
        auto line_of = [&](std::string_view channel, std::vector<std::string> extra) -> std::string {
            p.remove("target");
            std::vector<std::string> args{"build"};
            args.insert(args.end(), extra.begin(), extra.end());
            (void)p.mcpp(args);
            const auto ninja = build_ninja(p.at("target"));
            if (!ninja) return {};
            for (const auto& l : lines_of(s::read_file(*ninja)))
                if (l.starts_with(channel)) return l;
            return {};
        };
        auto normalise = [](std::string_view line) {
            std::vector<std::string> words;
            for (auto part : std::views::split(line, ' ')) {
                std::string w(part.begin(), part.end());
                if (w.empty() || w.contains("--target=") || w.contains("-fprebuilt-module-path=")) continue;
                words.push_back(std::move(w));
            }
            std::ranges::sort(words);
            return words;
        };
        int checked = 0;
        for (std::string tc : {"gcc", "llvm"}) {
            const auto ver = installed_version(p.dir(), tc);
            if (ver.empty()) {
                std::println("  SKIP  {} is not installed here", tc);
                continue;
            }
            p.write("mcpp.toml", std::format("[package]\nname    = \"idprobe\"\nversion = \"0.1.0\"\n\n[toolchain]\ndefault = \"{}@{}\"\n", tc, ver));
            const auto ht = jstr(s::json(p.mcpp({"why", "toolchain", "--toolchain", tc + "@" + ver, "--format", "json"}).out), {"data", "triple", "toolchain"});
            if (ht.empty()) {
                std::println("  SKIP  the query did not name a target for {}@{}", tc, ver);
                continue;
            }
            const auto implicit = line_of("ldflags", {});
            const auto explicit_ = line_of("ldflags", {"--target", ht});
            const auto implicit_cxx = line_of("cxxflags", {});
            const auto explicit_cxx = line_of("cxxflags", {"--target", ht});
            if (implicit.empty() || explicit_.empty()) {
                if (implicit.empty() && explicit_.empty()) {
                    std::println("  SKIP  {}@{} built nothing either way here", tc, ver);
                    continue;
                }
                ++checked;
                expect(false) << "FAIL: " << tc << "@" << ver << " — one spelling built and the other did not (implicit "
                              << (implicit.empty() ? "produced nothing" : "produced a link line") << ", explicit "
                              << (explicit_.empty() ? "produced nothing" : "produced a link line") << ")";
                continue;
            }
            ++checked;
            for (auto [channel, lhs, rhs] : {std::tuple{"ldflags", implicit, explicit_}, std::tuple{"cxxflags", implicit_cxx, explicit_cxx}}) {
                if (lhs.empty() || rhs.empty()) {
                    expect(false) << "FAIL: " << tc << "@" << ver << ": no " << channel << " line to compare";
                    continue;
                }
                expect(normalise(lhs) == normalise(rhs)) << "FAIL: " << tc << "@" << ver << ": naming " << ht << " changed the " << channel << " line\n  implicit: " << lhs << "\n  explicit: " << rhs;
            }
        }
        if (checked == 0) skip("no toolchain here produced a link line to compare");
    };

    "299 a request that named no c library resolves to a row that exists"_test[tag{"requires:unix-shell"}][tag{"requires:jq"}] = [] {
        // `--target aarch64-linux` resolves to a supported row of the system (the native GNU row
        // on an aarch64 Linux host); x86_64-linux stays gnu on an x86_64 Linux host; a written
        // `-gnu` keeps the GNU row.
        s::needs({"unix-shell", "jq"});
        s::project p({{"mcpp.toml", "[package]\nname    = \"reqprobe\"\nversion = \"0.1.0\"\n"},
                      {"src/main.cpp", "extern \"C\" int main(int, char**, char**) { return 0; }\n"}});
        auto why = [&](std::string_view t) { return s::json(p.mcpp({"why", "toolchain", "--target", std::string(t), "--format", "json"}).out); };
        auto reason_for = [&](std::string_view t) { return jstr(why(t), {"data", "reason"}, "-"); };
        auto resolved_row = [&](std::string_view t) -> std::string {
            const auto doc = why(t);
            if (auto r = jstr(doc, {"data", "triple", "llvm"}); !r.empty()) return r;
            std::string first;
            if (doc.is_object() && doc.contains("diagnostics") && doc["diagnostics"].is_array() && !doc["diagnostics"].empty())
                first = jstr(doc["diagnostics"][0], {"message"});
            first = first.substr(0, first.find('\n'));
            std::smatch m;
            static const std::regex quoted{R"(^[^']*'([^']*)'.*)"};
            static const std::regex section{R"(^\[target\.([^\]]*)\].*)"};
            if (std::regex_match(first, m, quoted)) return m[1].str();
            if (std::regex_match(first, m, section)) return m[1].str();
            return {};
        };
        auto message_of = [&](std::string_view t) { return messages(why(t)); };
        const auto r = reason_for("aarch64-linux");
        const auto row = resolved_row("aarch64-linux");
        auto is_aarch64_linux_row = [](std::string_view x) {
            return x == "aarch64-linux-gnu" || x == "aarch64-unknown-linux-gnu" || x == "aarch64-linux-musl" || x == "aarch64-unknown-linux-musl";
        };
        expect(fatal(r != "tier-planned" && r != "unknown-target")) << "FAIL: the tier gate still answers about the lexical fill (reason '" << r << "'); aarch64-linux-gnu is verified; completion must preserve its GNU identity\n" << message_of("aarch64-linux");
        if (r != "none" && r != "host-cannot-serve") {
            expect(fatal(is_aarch64_linux_row(row) && e2e::contains(message_of("aarch64-linux"), "could not be installed"))) << "FAIL: --target aarch64-linux refused for reason '" << r << "'\n" << message_of("aarch64-linux");
        }
        const bool aarch64_linux_host = s::on_linux() && uname_m() == "aarch64";
        if (aarch64_linux_host)
            expect(fatal(row == "aarch64-linux-gnu" || row == "aarch64-unknown-linux-gnu")) << "FAIL: aarch64-linux resolved to '" << row << "' on an aarch64 Linux host, not the GNU row";
        else
            expect(fatal(is_aarch64_linux_row(row))) << "FAIL: aarch64-linux resolved to '" << row << "', no aarch64 Linux row";
        expect(fatal(row.starts_with("aarch64"))) << "FAIL: aarch64-linux resolved to '" << row << "', which is not aarch64";
        const auto hostr = reason_for("x86_64-linux");
        const auto hostrow = resolved_row("x86_64-linux");
        if (s::on_linux() && uname_m() == "x86_64")
            expect(fatal(hostrow.ends_with("-gnu"))) << "FAIL: x86_64-linux resolved to '" << hostrow << "' on an x86_64 Linux host (reason '" << hostr << "')";
        else
            expect(fatal(std::regex_match(hostrow, std::regex{"x86_64.*linux.*"}))) << "FAIL: x86_64-linux resolved to '" << hostrow << "' (reason '" << hostr << "')";
        const auto gnu = resolved_row("aarch64-linux-gnu");
        expect(fatal(gnu == "aarch64-linux-gnu" || gnu == "aarch64-unknown-linux-gnu")) << "FAIL: written -gnu resolved to '" << gnu << "'";
    };

    "300 a registered family is not reported unknown"_test[tag{"requires:unix-shell"}][tag{"requires:jq"}] = [] {
        // A family the vocabulary registers (riscv64-linux, whose row riscv64-linux-musl is
        // planned) is refused as planned, naming the row that exists and no triple nobody wrote; a
        // typo is still unknown and keeps its suggestion. The rule is decided in the build's
        // preparation (its tier gate), which exports no function that answers it.
        s::needs({"unix-shell", "jq"});
        s::project p({{"mcpp.toml", "[package]\nname    = \"unkprobe\"\nversion = \"0.1.0\"\n"},
                      {"src/main.cpp", "extern \"C\" int main(int, char**, char**) { return 0; }\n"}});
        auto json_for = [&](std::string_view t) { return s::json(p.mcpp({"why", "toolchain", "--target", std::string(t), "--format", "json"}).out); };
        const auto j = json_for("riscv64-linux");
        const auto reason = jstr(j, {"data", "reason"}, "-");
        const auto msg = messages(j);
        expect(fatal(reason == "tier-planned")) << "FAIL: riscv64-linux refused as '" << reason << "', expected tier-planned\n" << msg;
        expect(fatal(e2e::contains(msg, "riscv64-linux-musl"))) << "FAIL: the refusal does not name riscv64-linux-musl\n" << msg;
        expect(fatal(s::lacks(msg, "riscv64-linux-gnu"))) << "FAIL: the refusal quotes 'riscv64-linux-gnu', which the reader never wrote";
        const auto j2 = json_for("x86_64-linux-mus");
        const auto reason2 = jstr(j2, {"data", "reason"}, "-");
        const auto msg2 = messages(j2);
        expect(fatal(reason2 == "unknown-target")) << "FAIL: a typo'd triple refused as '" << reason2 << "', expected unknown-target\n" << msg2;
        expect(fatal(e2e::contains(msg2, "x86_64-linux-musl"))) << "FAIL: the did-you-mean suggestion was lost\n" << msg2;
    };

    "1042 each flag names its layer and a request its selection"_test[tag{"requires:elf"}][tag{"requires:llvm"}] = [] {
        // `why flags` names the layer of each word (row, engine, package), and `why target
        // --format json` states the request, the identity and the candidates in order.
        s::needs({"elf", "llvm"});
        s::project p({{"mcpp.toml", "[package]\nname    = \"bare\"\nversion = \"0.1.0\"\n\n[build]\nsources = [\"src/*.c\"]\ncflags  = [\"-DBARE_APP=1\"]\n\n[targets.bare]\nkind = \"bin\"\nmain = \"src/main.c\"\n"},
                      {"src/main.c", "void _start(void) { __asm__ volatile(\"mov $60, %eax\\n mov $7, %edi\\n syscall\"); }\n"}});
        auto r = p.mcpp({"why", "flags", "main.c", "--target", "x86_64-none-elf"});
        expect(fatal(e2e::succeeded(r))) << "why flags failed: " << s::text(r);
        const auto& why = r.out;
        expect(fatal(s::has_line(why, R"(^  row +-march=x86-64 +\[x86_64-none-elf\])"))) << "the row's word does not name the row";
        expect(fatal(s::has_line(why, R"(^  row +-mno-red-zone +\[x86_64-none-elf\])"))) << "the row's flag does not name the row";
        expect(fatal(s::has_line(why, R"(^  engine +-ffreestanding)"))) << "the engine's convention does not name the engine";
        expect(fatal(s::has_line(why, R"(^  package +-DBARE_APP=1 +\[bare\])"))) << "the package's define does not name the package";
        expect(fatal(s::has_line(why, R"(^  row +-m elf_x86_64)"))) << "the link's emulation does not name the row";
        r = p.mcpp({"why", "target", "--target", "x86_64-windows", "--format", "json"});
        expect(fatal(e2e::succeeded(r))) << "why target failed: " << s::text(r);
        const auto doc = s::json(r.out);
        expect(fatal(doc.is_object() && doc.contains("data") && doc["data"].contains("target"))) << "why target --format json does not state the selection: " << r.out;
        const auto& t = doc["data"]["target"];
        expect(fatal(t.value("request", std::string()) == "x86_64-windows")) << t.dump();
        const auto identity = t.value("identity", std::string());
        expect(fatal(identity == "x86_64-windows-msvc" || identity == "x86_64-windows-musl" || identity == "x86_64-windows-gnu")) << t.dump();
        expect(fatal(t.contains("candidates") && t["candidates"].is_array() && !t["candidates"].empty())) << t.dump();
        expect(fatal(t["candidates"][0].value("identity", std::string()) == "x86_64-windows-msvc")) << t["candidates"].dump();
        bool chosen = false;
        for (const auto& c : t["candidates"])
            if (c.value("identity", std::string()) == identity && c.contains("provider") && t.contains("provider") && c["provider"] == t["provider"]) chosen = true;
        expect(fatal(chosen)) << t.dump();
    };
}};

}  // namespace
