// The packages of images: the openkal set as one tree of every architecture, single files and
// the polyglot file, packages that merge and describe themselves, a WebAssembly package that is
// neither an entry nor a single file, the Windows launcher over any platform release, and the
// runners `--systems all` carries (formerly tests/e2e/955, 956, 957, 961, 962, 992, 995, 1078).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

#if defined(__x86_64__) || defined(_M_X64)
constexpr std::string_view host_arch = "x86_64", other_arch = "aarch64";
#elif defined(__aarch64__) || defined(_M_ARM64)
constexpr std::string_view host_arch = "aarch64", other_arch = "x86_64";
#else
constexpr std::string_view host_arch = "", other_arch = "";
#endif

// The script's `case "$(uname -m)"`: a machine with no ok-image row skips.
void needs_image_row() {
    if (host_arch.empty()) skip("no ok-image row for this machine");
}

std::string posix_system() { return s::on_linux() ? "linux" : s::on_macos() ? "macos" : ""; }

std::uint32_t machine_of(std::string_view arch) { return arch == "x86_64" ? s::elf_machine::x86_64 : s::elf_machine::aarch64; }

bool executable(const fs::path& f) {
    std::error_code ec;
    return fs::is_regular_file(f, ec) && (fs::status(f, ec).permissions() & fs::perms::owner_exec) != fs::perms::none;
}

// `env -i HOME="$HOME" PATH=/usr/bin:/bin <program> <args>`: the program with nothing installed.
e2e::outcome run_bare(const fs::path& program, std::vector<std::string> args) {
    e2e::command c{.argv = {s::on_path("env").value_or("/usr/bin/env").string(), "-i", "HOME=" + s::env("HOME").value_or(""), "PATH=/usr/bin:/bin", program.string()}};
    for (auto& a : args) c.argv.push_back(std::move(a));
    return e2e::run(c);
}

fs::path runner_of(const s::project& p, std::string_view target) {
    auto r = p.mcpp({"self", "env", "--target", std::string(target), "--format", "json"});
    expect(fatal(e2e::succeeded(r))) << "self env: " << s::text(r);
    const auto d = s::json(r.out)["data"];
    expect(fatal(d["runner"].is_object() && d["runner"]["program"].is_string())) << "the platform's runner was not located: " << r.out;
    return d["runner"]["program"].get<std::string>();
}

std::vector<std::string> names_in(const fs::path& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    if (fs::is_directory(dir, ec))
        for (const auto& e : fs::directory_iterator(dir)) out.push_back(e.path().filename().string());
    std::ranges::sort(out);
    return out;
}

std::vector<std::string> strings(const nlohmann::json& a) {
    std::vector<std::string> out;
    for (const auto& x : a) out.push_back(x.get<std::string>());
    return out;
}

void write_json(const fs::path& f, const nlohmann::json& d, int indent = -1) {
    std::ofstream(f, std::ios::binary | std::ios::trunc) << d.dump(indent);
}

// `tar czf <archive> -C <dir> <member>`, by the machine's tar.
void tar_create(const fs::path& archive, const fs::path& dir, std::string_view member) {
    const auto tar = s::on_path("tar");
    expect(fatal(tar.has_value())) << "tar is not on PATH";
    auto r = s::run(*tar, {"czf", archive.string(), "-C", dir.string(), std::string(member)});
    expect(fatal(e2e::succeeded(r))) << "tar: " << s::text(r);
}

// A zip's files extracted (`python3 -I -m zipfile -e`): the support reads a zip's entries, not
// its deflated contents, and these scripts read them with python3's zipfile.
void zip_extract(const fs::path& zip, const fs::path& into) {
    const auto python = s::on_path("python3");
    expect(fatal(python.has_value())) << "python3 is needed to read the zip";
    auto r = s::run(*python, {"-I", "-m", "zipfile", "-e", zip.string(), into.string()});
    expect(fatal(e2e::succeeded(r))) << "zipfile -e: " << s::text(r);
}

// Every entry under `root` writable again (a single file's cache is made read-only), so that the
// case's directory can be removed whichever way the case ends.
struct writable_on_exit {
    fs::path root;
    ~writable_on_exit() {
        std::error_code ec;
        if (!fs::exists(root, ec)) return;
        fs::permissions(root, fs::perms::owner_all, fs::perm_options::add, ec);
        for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec); it != fs::recursive_directory_iterator(); it.increment(ec))
            if (!it->is_symlink(ec)) fs::permissions(it->path(), fs::perms::owner_all, fs::perm_options::add, ec);
    }
};

constexpr std::string_view caught_main = "import std;\nint main(int argc, char** argv) {\n    try { throw std::runtime_error(\"caught\"); }\n    catch (const std::exception& e) { std::println(\"image: {} ({} args)\", e.what(), argc); }\n    return 0;\n}\n";

E2E trees{"trees", [] {
    "955 the openkal set is one tree of every architecture"_test[tag{"requires:llvm"}][tag{"requires:python3"}][tag{"requires:unix-shell"}] = [] {
        // A `pack --target openkal` writes <name>-<version>-openkal/ with both images, the
        // launchers and metadata naming legs, runner and entries; B the launcher runs this
        // machine's image through the runner on PATH; C self-contained carries the runner per
        // system and architecture and runs with nothing installed; D one target is packed alone
        // in the same layout; E (Linux) `--systems linux,macos,windows` carries a Mach-O and a
        // PE runner made here, none with its own package description.
        s::needs({"llvm", "python3", "unix-shell"});
        needs_image_row();
        const auto os = posix_system();
        if (os.empty()) skip("a POSIX launcher");
        const std::string t = std::string(host_arch) + "-openkal";
        s::project app({{"mcpp.toml", "[package]\nname    = \"uni\"\nversion = \"0.1.0\"\n"},
                        {"src/main.cpp", "import std;\nint main(int argc, char** argv) {\n    try { throw std::runtime_error(\"universal\"); }\n    catch (const std::exception& e) { std::println(\"{} {} args\", e.what(), argc); }\n    return 0;\n}\n"}});
        // A
        auto r = app.mcpp({"pack", "--target", "openkal", "--pkg-format", "dir"});
        expect(fatal(e2e::succeeded(r))) << "A: pack: " << s::text(r);
        const auto p = app.at("target/dist/uni-0.1.0-openkal");
        for (std::string_view f : {"lib/x86_64/uni.oke", "lib/aarch64/uni.oke", "bin/uni", "bin/uni.exe", "mcpp-package.json"})
            expect(fatal(fs::is_regular_file(p / f))) << "A: the package has no " << f << ": " << s::text(r);
        auto d = s::json_file(p / "mcpp-package.json");
        auto legs = strings(d["legs"]);
        std::ranges::sort(legs);
        expect(fatal(legs == std::vector<std::string>{"aarch64-openkal", "x86_64-openkal"})) << "A: " << d["legs"].dump();
        expect(fatal(d["launch"]["runner"][0].get<std::string>().starts_with("okrun"))) << "A: " << d["launch"].dump();
        expect(fatal(d["launch"]["entry"] == nlohmann::json{{"x86_64", "lib/x86_64/uni.oke"}, {"aarch64", "lib/aarch64/uni.oke"}})) << "A: " << d["launch"].dump();
        for (std::string_view arch : {"x86_64", "aarch64"}) {
            const auto h = s::read_header(p / std::format("lib/{}/uni.oke", arch));
            expect(fatal(h.kind == s::format::elf && h.machine == machine_of(arch))) << "A: lib/" << arch << "/uni.oke is not an image of its architecture";
        }
        std::map<std::string, std::string> roles;
        for (const auto& f : d["files"]) roles[f["path"].get<std::string>()] = f["role"].get<std::string>();
        expect(fatal(roles["bin/uni"] == "launcher" && roles["lib/x86_64/uni.oke"] == "image")) << "A: " << d["files"].dump();
        // B
        const auto okrun = runner_of(app, t);
        expect(fatal(executable(okrun))) << "B: the platform's runner was not located (" << okrun << ")";
        auto ran = s::run(p / "bin/uni", {"one", "two"}, {}, {{"PATH", okrun.parent_path().string() + ":" + s::env("PATH").value_or("")}});
        expect(fatal(e2e::succeeded(ran))) << "B: the launcher: " << s::text(ran);
        expect(fatal(e2e::contains(s::text(ran), "universal 3 args"))) << "B: the image's output through the launcher: " << s::text(ran);
        // C: this machine's architecture's runner is always there; the other one where this mcpp
        // builds programs for it.
        r = app.mcpp({"pack", "--target", "openkal", "--mode", "self-contained", "--pkg-format", "dir", "-o", "sc"});
        expect(fatal(e2e::succeeded(r))) << "C: pack --mode self-contained: " << s::text(r);
        const auto sc = app.at("target/dist/sc");
        expect(fatal(fs::is_regular_file(sc / std::format("libexec/okrun/{}-{}/okrun", os, host_arch)))) << "C: no runner for " << os << "-" << host_arch << ": " << s::text(r);
        if (!fs::is_regular_file(sc / std::format("libexec/okrun/{}-{}/okrun", os, other_arch)))
            expect(fatal(e2e::contains(s::text(r), std::format("no runner for {}-{}", os, other_arch)))) << "C: the runner for " << os << "-" << other_arch << " is missing and not said: " << s::text(r);
        ran = run_bare(sc / "bin/uni", {"x"});
        expect(fatal(e2e::succeeded(ran))) << "C: the self-contained launcher: " << s::text(ran);
        expect(fatal(e2e::contains(s::text(ran), "universal 2 args"))) << "C: the self-contained package's output: " << s::text(ran);
        d = s::json_file(sc / "mcpp-package.json");
        const auto carried = names_in(sc / "libexec/okrun");
        expect(fatal(d["mode"] == "self-contained" && strings(d["systems"]) == carried && std::ranges::contains(carried, std::format("{}-{}", os, host_arch)))) << "C: systems: " << d.dump();
        // D
        r = app.mcpp({"pack", "--target", t, "--pkg-format", "dir", "-o", "one"});
        expect(fatal(e2e::succeeded(r))) << "D: one target: " << s::text(r);
        expect(fatal(fs::is_regular_file(app.at(std::format("target/dist/one/lib/{}/uni.oke", host_arch))))) << "D: the target's leg is not in lib/" << host_arch << "/: " << s::text(r);
        expect(fatal(fs::is_regular_file(app.at("target/dist/one/bin/uni")))) << "D: the one-leg package has no launcher: " << s::text(r);
        expect(fatal(!fs::is_directory(app.at(std::format("target/dist/one/lib/{}", other_arch))))) << "D: one target packed the other architecture";
        // E
        if (os == "linux") {
            r = app.mcpp({"pack", "--target", "openkal", "--mode", "self-contained", "--systems", "linux,macos,windows", "--pkg-format", "dir", "-o", "uni-0.1.0-all"});
            expect(fatal(e2e::succeeded(r))) << "E: --systems linux,macos,windows: " << s::text(r);
            const auto root = app.at("target/dist/uni-0.1.0-all/libexec/okrun");
            expect(fatal(s::read_file(root / "macos-aarch64/okrun").starts_with("\xcf\xfa\xed\xfe"))) << "E: macos-aarch64 is not a Mach-O: " << s::text(r);
            expect(fatal(s::read_file(root / "windows-x86_64/okrun.exe").starts_with("MZ"))) << "E: windows-x86_64 is not a PE: " << s::text(r);
            for (const auto& e : fs::recursive_directory_iterator(root))
                expect(fatal(e.path().filename() != "mcpp-package.json")) << "E: a runner carries its package description: " << e.path();
            expect(fatal(e2e::contains(s::text(r), "no runner for macos-x86_64"))) << "E: the planned row is not said: " << s::text(r);
        }
    };

    "956 a single file runs without unpacking"_test[tag{"requires:unix-shell"}][tag{"requires:python3"}] = [] {
        // A a native single file runs with its arguments and exit status, extracting its tree
        // into the cache once; B (Linux) `--linkage static` makes a file with no interpreter;
        // C `--compress xz` is refused naming zstd; D an image's single file runs and writes
        // nothing into the cache; E `-o` naming one file with several systems, and a system of
        // an unbuilt row, are refused before a build.
        s::needs({"unix-shell", "python3"});
        const auto tmp = s::case_dir();
        const writable_on_exit guard{tmp};
        // The user's cache, isolated: $XDG_CACHE_HOME on Linux; on macOS ~/Library/Caches, so the
        // files run with a home of their own (mcpp keeps the real one).
        fs::path cache;
        std::vector<std::pair<std::string, std::optional<std::string>>> file_env;
        s::options o;
        if (s::on_macos()) {
            cache = tmp / "home/Library/Caches";
            fs::create_directories(cache);
            file_env = {{"HOME", (tmp / "home").string()}};
        } else {
            cache = tmp / "cache";
            file_env = {{"XDG_CACHE_HOME", cache.string()}};
            o.env = file_env;
        }
        s::project app(tmp / "app", {{"mcpp.toml", "[package]\nname    = \"one\"\nversion = \"0.1.0\"\n"},
                                     {"src/main.cpp", "#include <cstdio>\nint main(int argc, char** argv) { std::printf(\"one file: %d args, last %s\\n\", argc, argv[argc - 1]); return 3; }\n"}});
        // A
        auto r = app.mcpp({"pack", "--pkg-format", "single-file"}, o);
        expect(fatal(e2e::succeeded(r))) << "A: pack: " << s::text(r);
        std::optional<fs::path> f;
        for (const auto& c : s::glob(app.at("target/dist"), "one-0.1.0-*"))
            if (executable(c)) { f = c; break; }
        expect(fatal(f.has_value())) << "A: no single file under target/dist: " << s::text(r);
        auto ran = s::run(*f, {"alpha", "beta"}, {}, file_env);
        expect(fatal(ran.started && ran.status == 3)) << "A: the exit status is " << ran.status << ", not the program's 3: " << s::text(ran);
        expect(fatal(e2e::contains(s::text(ran), "one file: 3 args, last beta"))) << "A: the program's output: " << s::text(ran);
        const auto trees = s::glob(cache / "mcpp/single-file", "*");
        expect(fatal(trees.size() == 1u)) << "A: the first run left " << trees.size() << " trees in the cache";
        const auto stamp_of = [&] {
            const auto c = s::glob(cache / "mcpp/single-file", "*/.mcpp-complete");
            expect(fatal(!c.empty())) << "A: no .mcpp-complete";
            return fs::last_write_time(c.front());
        };
        const auto stamp = stamp_of();
        ran = s::run(*f, {"gamma"}, {}, file_env);
        expect(fatal(e2e::contains(s::text(ran), "last gamma"))) << "A: the second run: " << s::text(ran);
        expect(fatal(stamp_of() == stamp)) << "A: the second run extracted the tree again";
        // B
        if (s::on_linux()) {
            r = app.mcpp({"pack", "--pkg-format", "single-file", "--linkage", "static", "-o", "one-static"}, o);
            expect(fatal(e2e::succeeded(r))) << "B: --linkage static: " << s::text(r);
            const auto h = s::read_header(app.at("target/dist/one-static"));
            expect(fatal(h.kind == s::format::elf && !h.elf_interpreter)) << "B: the static file has an interpreter: " << h.interpreter;
            ran = s::run(app.at("target/dist/one-static"), {"z"}, {}, file_env);
            expect(fatal(e2e::contains(s::text(ran), "last z"))) << "B: the static single file: " << s::text(ran);
        }
        // C
        r = app.mcpp({"pack", "--pkg-format", "single-file", "--compress", "xz"}, o);
        expect(fatal(!e2e::succeeded(r))) << "C: xz was accepted: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "zstd"))) << "C: the refusal does not name the codec: " << s::text(r);
        // D, E: where this machine has an ok-image row and mcpp lists an llvm toolchain
        if (host_arch.empty()) return;
        const std::string t = std::string(host_arch) + "-openkal";
        if (!e2e::contains(s::mcpp({"toolchain", "list", "--format", "json"}, tmp, o).out, "llvm")) return;
        s::project img(tmp / "img", {{"mcpp.toml", "[package]\nname    = \"img\"\nversion = \"0.1.0\"\n"}, {"src/main.cpp", "import std;\nint main(int argc, char**) { std::println(\"image file: {} args\", argc); }\n"}});
        r = img.mcpp({"pack", "--target", t, "--pkg-format", "single-file"}, o);
        expect(fatal(e2e::succeeded(r))) << "D: pack: " << s::text(r);
        const auto files = s::glob(img.at("target/dist"), "img-0.1.0-*");
        expect(fatal(!files.empty())) << "D: no single file: " << s::text(r);
        const auto count = [&] {
            std::size_t n = 0;
            std::error_code ec;
            if (fs::is_directory(cache, ec))
                for (const auto& e : fs::recursive_directory_iterator(cache))
                    if (e.is_regular_file()) ++n;
            return n;
        };
        const auto before = count();
        ran = s::run(files.front(), {"x", "y"}, {}, file_env);
        expect(fatal(e2e::succeeded(ran))) << "D: the image's single file: " << s::text(ran);
        expect(fatal(e2e::contains(s::text(ran), "image file: 3 args"))) << "D: the output: " << s::text(ran);
        expect(fatal(count() == before)) << "D: the image's single file wrote into the cache";
        // E
        r = img.mcpp({"pack", "--target", t, "--pkg-format", "single-file", "--systems", "linux,macos", "-o", "one-file"}, o);
        expect(fatal(!e2e::succeeded(r))) << "E: -o with two systems was accepted: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "single files (one per leg and system)"))) << "E: the refusal: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "Compiling"))) << "E: refused after compiling: " << s::text(r);
        r = img.mcpp({"pack", "--target", "x86_64-openkal", "--pkg-format", "single-file", "--systems", "linux,macos"}, o);
        expect(fatal(!e2e::succeeded(r))) << "E: a single file for x86_64-macos was accepted: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "x86_64-macos"))) << "E: the refusal does not name the row: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "--systems linux"))) << "E: the refusal does not give the systems that are built: " << s::text(r);
    };

    "957 packages merge and describe themselves"_test[tag{"requires:llvm"}][tag{"requires:python3"}][tag{"requires:unix-shell"}] = [] {
        // A `--merge a --merge b` (b: a's runner moved to another system's directory) writes one
        // package named after the output, the union of legs, systems and files with their
        // digests; D an archive merges as its directory does; B a file of both with different
        // digests, and different versions, are refused; C `emit xpkg --package` of a package
        // of images is one asset per system its `runs_on` names (or refused when empty), and an
        // archive without its own --package is refused.
        s::needs({"llvm", "python3", "unix-shell"});
        needs_image_row();
        const auto tmp = s::case_dir();
        s::project app(tmp / "app", {{"mcpp.toml", "[package]\nname    = \"mrg\"\nversion = \"0.1.0\"\n"}, {"src/main.cpp", "import std;\nint main() { std::println(\"merged\"); }\n"}});
        auto r = app.mcpp({"pack", "--target", "openkal", "--mode", "self-contained", "--pkg-format", "dir", "-o", "here"});
        expect(fatal(e2e::succeeded(r))) << "pack: " << s::text(r);
        // The same package as another system packs it: its runner in that system's directory.
        const auto there = tmp / "there";
        fs::copy(app.at("target/dist/here"), there, fs::copy_options::recursive | fs::copy_options::copy_symlinks);
        {
            auto d = s::json_file(there / "mcpp-package.json");
            const auto okrun_dir = there / "libexec/okrun";
            std::map<std::string, std::string> moved;
            for (const auto& sysdir : names_in(okrun_dir)) {
                auto other = sysdir;
                for (std::string_view from : {"linux-", "macos-"})
                    if (auto at = other.find(from); at != std::string::npos) other.replace(at, from.size(), "other-");
                fs::rename(okrun_dir / sysdir, okrun_dir / other);
                moved[sysdir] = other;
            }
            for (auto& f : d["files"]) {
                auto path = f["path"].get<std::string>();
                for (const auto& [a, b] : moved)
                    if (auto at = path.find(std::format("libexec/okrun/{}/", a)); at != std::string::npos) path.replace(at, std::format("libexec/okrun/{}/", a).size(), std::format("libexec/okrun/{}/", b));
                f["path"] = path;
            }
            std::vector<std::string> systems;
            for (const auto& [_, b] : moved) systems.push_back(b);
            std::ranges::sort(systems);
            d["systems"] = systems;
            write_json(there / "mcpp-package.json", d, 2);
        }
        // A
        r = app.mcpp({"pack", "--merge", "target/dist/here", "--merge", there.string(), "-o", (tmp / "merged.zip").string()});
        expect(fatal(e2e::succeeded(r))) << "A: --merge: " << s::text(r);
        const auto entries = s::zip_entries(tmp / "merged.zip");
        expect(fatal(!entries.empty() && std::ranges::all_of(entries, [](const s::zip_entry& e) { return e.name.starts_with("merged/"); }))) << "A: the package is not named after the output";
        zip_extract(tmp / "merged.zip", tmp / "merged-zip");
        auto d = s::json_file(tmp / "merged-zip/merged/mcpp-package.json");
        const auto systems = strings(d["systems"]);
        expect(fatal(systems.size() == 4u && std::ranges::any_of(systems, [](const std::string& x) { return x.starts_with("other-"); }))) << "A: " << d["systems"].dump();
        auto legs = strings(d["legs"]);
        std::ranges::sort(legs);
        expect(fatal(legs == std::vector<std::string>{"aarch64-openkal", "x86_64-openkal"})) << "A: " << d["legs"].dump();
        for (const auto& f : d["files"])
            expect(fatal(s::sha256(s::read_file(tmp / "merged-zip/merged" / f["path"].get<std::string>())) == f["sha256"].get<std::string>())) << "A: " << f["path"].dump();
        // D (before B changes the second package)
        {
            const auto staged = tmp / "stage";
            fs::create_directories(staged);
            fs::copy(there, staged / "mrg-0.1.0-openkal", fs::copy_options::recursive | fs::copy_options::copy_symlinks);
            tar_create(tmp / "there.tar.gz", staged, "mrg-0.1.0-openkal");
        }
        r = app.mcpp({"pack", "--merge", "target/dist/here", "--merge", (tmp / "there.tar.gz").string(), "--pkg-format", "dir", "-o", (tmp / "merged-dir").string()});
        expect(fatal(e2e::succeeded(r))) << "D: --merge of a directory and an archive: " << s::text(r);
        d = s::json_file(tmp / "merged-dir/mcpp-package.json");
        expect(fatal(d["systems"].size() == 4u)) << "D: " << d["systems"].dump();
        for (const auto& f : d["files"])
            expect(fatal(s::sha256(s::read_file(tmp / "merged-dir" / f["path"].get<std::string>())) == f["sha256"].get<std::string>())) << "D: " << f["path"].dump();
        // B
        {
            const auto img = there / "lib/x86_64/mrg.oke";
            std::ofstream(img, std::ios::binary | std::ios::app) << '\0';
            auto m = s::json_file(there / "mcpp-package.json");
            for (auto& f : m["files"])
                if (f["path"] == "lib/x86_64/mrg.oke") f["sha256"] = s::sha256(s::read_file(img));
            write_json(there / "mcpp-package.json", m, 2);
        }
        r = app.mcpp({"pack", "--merge", "target/dist/here", "--merge", there.string(), "-o", (tmp / "bad.zip").string()});
        expect(fatal(!e2e::succeeded(r))) << "B: two digests of one image were merged: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "lib/x86_64/mrg.oke"))) << "B: the refusal does not name the file: " << s::text(r);
        {
            auto m = s::json_file(there / "mcpp-package.json");
            m["version"] = "0.2.0";
            write_json(there / "mcpp-package.json", m);
        }
        r = app.mcpp({"pack", "--merge", "target/dist/here", "--merge", there.string(), "-o", (tmp / "bad2.zip").string()});
        expect(fatal(!e2e::succeeded(r))) << "B: two versions were merged: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "version"))) << "B: the refusal does not name the version: " << s::text(r);
        // C
        r = app.mcpp({"pack", "--target", "openkal", "--pkg-format", "zip", "-o", "mrg-0.1.0-openkal.zip"});
        expect(fatal(e2e::succeeded(r))) << "C: pack: " << s::text(r);
        const auto zip = app.at("target/dist/mrg-0.1.0-openkal.zip");
        const auto emitted = app.mcpp({"emit", "xpkg", "--package", "target/dist/mrg-0.1.0-openkal.zip", "--url", "https://example.org/v0.1.0"});
        zip_extract(zip, tmp / "c-zip");
        const auto m = s::json_file(tmp / "c-zip/mrg-0.1.0-openkal/mcpp-package.json");
        std::vector<std::string> pairs;
        if (m["runs_on"]["with"].contains("okrun")) pairs = strings(m["runs_on"]["with"]["okrun"]);
        if (pairs.empty()) {
            expect(fatal(emitted.started && emitted.status != 0 && e2e::contains(emitted.err, "runs_on is empty"))) << "C: " << emitted.status << " " << emitted.err;
        } else {
            expect(fatal(e2e::succeeded(emitted))) << "C: " << emitted.err;
            const auto& text = emitted.out;
            const auto sha = s::sha256(s::read_file(zip));
            for (const auto& [system, key] : {std::pair{"linux", "linux"}, std::pair{"macos", "macosx"}, std::pair{"windows", "windows"}}) {
                std::vector<std::string> archs;
                for (const auto& x : pairs)
                    if (x.starts_with(std::string(system) + "-")) archs.push_back(x.substr(x.find('-') + 1));
                std::smatch block;
                const bool found = std::regex_search(text, block, std::regex{std::string(key) + R"( = \{([\s\S]*?)\n        \},)"});
                if (archs.empty()) {
                    expect(fatal(!found)) << "C: " << system << " is described without a runner pair: " << text;
                    continue;
                }
                expect(fatal(found)) << "C: no block for " << system << ": " << text;
                const auto b = block[1].str();
                expect(fatal(e2e::contains(b, "https://example.org/v0.1.0/mrg-0.1.0-openkal.zip") && e2e::contains(b, sha))) << "C: " << system << ": " << b;
                expect(fatal(e2e::contains(b, R"(deps = { "xim:okrun" })"))) << "C: " << system << ": " << b;
                for (const auto& a : archs) expect(fatal(e2e::contains(b, std::format("[\"{}\"]", a)))) << "C: " << system << " lacks " << a << ": " << b;
            }
            expect(fatal(e2e::contains(text, R"(xvm.add("mrg")"))) << "C: " << text;
            expect(fatal(e2e::contains(text, "local top = pkginfo.install_file()") && e2e::contains(text, R"({ ".zip" })"))) << "C: " << text;
        }
        r = app.mcpp({"emit", "xpkg", "--package", "target/dist/mrg-0.1.0-openkal.zip", "target/dist/mrg-0.1.0-openkal.zip", "--url", "https://example.org/v0.1.0"});
        expect(fatal(!e2e::succeeded(r))) << "C: an archive without its own --package was taken: " << r.out;
        expect(fatal(e2e::contains(r.err, "each archive is its own --package"))) << "C: the refusal does not say how: " << r.err;
    };

    "961 a single file carries the image in a program of its architecture"_test[tag{"requires:llvm"}][tag{"requires:python3"}] = [] {
        // A the other architecture's single file is an ELF of that architecture (run by its
        // qemu where present); B `--list-formats` lists the platform's `okb` for the target and
        // not for this machine; C the self-contained tree of one target is described for its
        // pair, and the vendored tree as its runner's coverage says.
        s::needs({"llvm", "python3"});
        needs_image_row();
        const std::string system = s::on_macos() ? "macos" : s::on_windows() ? "windows" : "linux";
        const std::string xsystem = s::on_macos() ? "macosx" : system;
        const std::string ot = std::string(other_arch) + "-openkal";
        s::project app({{"mcpp.toml", "[package]\nname    = \"app\"\nversion = \"0.1.0\"\n"}, {"src/main.cpp", caught_main}});
        // A
        auto r = app.mcpp({"pack", "--target", ot, "--pkg-format", "single-file", "-o", "m3-file"});
        expect(fatal(e2e::succeeded(r))) << "A: pack: " << s::text(r);
        const auto f = app.at("target/dist/m3-file");
        expect(fatal(fs::is_regular_file(f))) << "A: no single file: " << s::text(r);
        const auto h = s::read_header(f);
        expect(fatal(h.kind == s::format::elf && h.machine == machine_of(other_arch))) << "A: the carrier is a program of machine " << h.machine << ", the image is " << other_arch;
        expect(fatal(e2e::contains(s::text(r), std::format("into a program of linux-{}", other_arch)))) << "A: the Packing line names another architecture: " << s::text(r);
        auto qemu = s::on_path(std::format("qemu-{}", other_arch));
        if (!qemu) qemu = s::on_path(std::format("qemu-{}-static", other_arch));
        if (qemu) {
            auto ran = s::run(*qemu, {f.string(), "x"});
            expect(fatal(e2e::succeeded(ran))) << "A: the program under " << *qemu << ": " << s::text(ran);
            expect(fatal(e2e::contains(s::text(ran), "image: caught (2 args)"))) << "A: the program's image under " << *qemu << ": " << s::text(ran);
        }
        // B
        r = app.mcpp({"pack", "--target", ot, "--list-formats"});
        expect(fatal(e2e::succeeded(r))) << "B: --list-formats --target " << ot << ": " << s::text(r);
        std::optional<nlohmann::json> okb;
        for (const auto& x : s::json(r.out)["data"]["formats"])
            if (x["name"] == "okb") okb = x;
        expect(fatal(okb.has_value())) << "B: no okb for the target: " << r.out;
        expect(fatal(std::ranges::any_of(strings((*okb)["providers"]), [](const std::string& x) { return x.ends_with("openkal-platform"); }))) << "B: " << okb->dump();
        r = app.mcpp({"pack", "--list-formats"});
        expect(fatal(e2e::succeeded(r))) << "B: --list-formats: " << s::text(r);
        for (const auto& x : s::json(r.out)["data"]["formats"]) expect(fatal(x["name"] != "okb")) << "B: okb is listed for this machine: " << r.out;
        // C
        r = app.mcpp({"pack", "--target", ot, "--mode", "self-contained", "--pkg-format", "dir", "-o", "m3"});
        expect(fatal(e2e::succeeded(r))) << "C: pack: " << s::text(r);
        const auto pair = std::format("{}-{}", system, other_arch);
        for (const auto& file : {std::format("lib/{}/app.oke", other_arch), std::string("bin/app"), std::format("libexec/okrun/{}/okrun", pair)})
            expect(fatal(fs::is_regular_file(app.at("target/dist/m3/" + file)))) << "C: the tree has no " << file << ": " << s::text(r);
        const auto d = s::json_file(app.at("target/dist/m3/mcpp-package.json"));
        expect(fatal(strings(d["systems"]) == std::vector<std::string>{pair})) << "C: " << d["systems"].dump();
        expect(fatal(strings(d["runs_on"]["self"]) == std::vector<std::string>{pair})) << "C: " << d["runs_on"].dump();
        expect(fatal(d["launch"]["entry"] == nlohmann::json{{std::string(other_arch), std::format("lib/{}/app.oke", other_arch)}})) << "C: " << d["launch"].dump();
        tar_create(app.at("m3.tar.gz"), app.at("target/dist"), "m3");
        r = app.mcpp({"emit", "xpkg", "--package", "m3.tar.gz", "--url", "https://example.org/v0.1.0"});
        expect(fatal(e2e::succeeded(r))) << "C: emit xpkg of the self-contained tree: " << r.err;
        expect(fatal(s::has_line(r.out, std::format("^        {} = \\{{", xsystem)))) << "C: the entry is not under " << xsystem << ": " << r.out;
        expect(fatal(e2e::contains(r.out, std::format("archs = {{\"{}\"}}", other_arch)))) << "C: the entry's architecture is not " << other_arch << ": " << r.out;
        expect(fatal(s::count_matching(r.out, R"(^        (linux|macosx|windows) = [{])") == 1u)) << "C: the entry names more than one system: " << r.out;
        r = app.mcpp({"pack", "--target", ot, "-o", "bare.tar.gz"});
        expect(fatal(e2e::succeeded(r))) << "C: the image's vendored package: " << s::text(r);
        const auto emitted = app.mcpp({"emit", "xpkg", "--package", "target/dist/bare.tar.gz", "--url", "https://example.org/v0.1.0"});
        s::tar_extract(app.at("target/dist/bare.tar.gz"), s::case_dir() / "bare-x", {"bare/mcpp-package.json"});
        const auto bare = s::json_file(s::case_dir() / "bare-x/bare/mcpp-package.json");
        expect(fatal(bare["runs_on"]["self"].empty())) << "C: " << bare["runs_on"].dump();
        if (!bare["runs_on"]["with"].contains("okrun") || bare["runs_on"]["with"]["okrun"].empty())
            expect(fatal(emitted.started && emitted.status != 0 && e2e::contains(emitted.err, "runs_on is empty"))) << "C: " << emitted.err;
        else
            expect(fatal(e2e::succeeded(emitted) && e2e::contains(emitted.out, R"(deps = { "xim:okrun" })"))) << "C: " << emitted.err << emitted.out;
    };

    "962 a webassembly package is neither an xlings entry nor a single file"_test[tag{"requires:unix-shell"}] = [] {
        // A `emit xpkg --package` of a wasm32-emscripten package exits 2 naming the package,
        // its triple and `dist-web`, writing no entry; B where an emsdk payload is installed,
        // `--pkg-format single-file` for wasm32-emscripten is refused before a build naming both
        // choices and the formats that serve the row.
        s::needs({"unix-shell"});
        const auto tmp = s::case_dir();
        const std::string top = "web-0.1.0-wasm32-emscripten";
        s::project(tmp / top, {{"bin/web.js", "console.log(\"web\");\n"},
                               {"bin/web.wasm", std::string_view("\0asm\1\0\0\0", 8)},
                               {"mcpp-package.json", R"({"schema": 1, "name": "web", "version": "0.1.0", "platform": null, "linkage": "declared",
 "mode": "vendored", "legs": ["wasm32-emscripten"], "systems": [],
 "launch": {"program": "bin/web.js"},
 "files": [{"path": "bin/web.js", "role": "program", "leg": "wasm32-emscripten", "sha256": "0"},
           {"path": "bin/web.wasm", "role": "program", "leg": "wasm32-emscripten", "sha256": "0"}]}
)"}});
        tar_create(tmp / (top + ".tar.gz"), tmp, top);
        auto r = s::mcpp({"emit", "xpkg", "--package", top + ".tar.gz", "--url", "https://example.org/v0.1.0"}, tmp);
        expect(fatal(r.started && r.status == 2)) << "A: emit xpkg of a wasm package exited " << r.status << ", not 2: " << s::text(r);
        expect(fatal(e2e::contains(r.err, top + ".tar.gz is a package of wasm32-emscripten, which is no program of Linux, macOS or Windows"))) << "A: the refusal does not name the package and its triple: " << r.err;
        expect(fatal(e2e::contains(r.err, "dist-web"))) << "A: the refusal does not name the route of the web: " << r.err;
        expect(fatal(r.out.empty())) << "A: an entry was written: " << r.out;
        bool emsdk = false;
        for (const auto& root : {s::shared_home() / "registry/data/xpkgs/xim-x-emsdk", fs::path(s::env("HOME").value_or("")) / ".xlings/data/xpkgs/xim-x-emsdk"})
            for (const auto& d : s::glob(root, "*/emscripten"))
                emsdk = emsdk || (fs::is_regular_file(d / "em++") && (fs::status(d / "em++").permissions() & fs::perms::owner_exec) != fs::perms::none);
        if (!emsdk) return;  // as the script: B is not run without an emsdk payload
        s::project web(tmp / "web", {{"mcpp.toml", "[package]\nname    = \"web\"\nversion = \"0.1.0\"\n"}, {"src/main.cpp", "import std;\nint main() { std::println(\"web\"); }\n"}});
        r = web.mcpp({"pack", "--target", "wasm32-emscripten", "--pkg-format", "single-file"});
        expect(fatal(r.started && r.status == 2)) << "B: single-file of a wasm program exited " << r.status << ", not 2: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "single-file makes an executable of one system, and wasm32-emscripten builds a WebAssembly module"))) << "B: the refusal does not name both choices: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "use: --pkg-format tar | zip | dir"))) << "B: the refusal names no way out: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "Compiling"))) << "B: refused after compiling: " << s::text(r);
    };

    "992 a polyglot file runs on every system it holds"_test[tag{"requires:llvm"}][tag{"requires:unix-shell"}] = [] {
        // A `--pkg-format polyglot -o out/` writes one PE whose DOS header is a script; B `sh`
        // runs this machine's program, placing it in the cache once, and bash runs it too; C a
        // damaged file is refused (127) placing nothing; D a linked cache directory is refused
        // (127); E without Windows the file is a script run directly; F `mcpp run --pkg-format
        // polyglot` runs it through sh; G the refusals; H (wine) it runs as a Windows program.
        s::needs({"llvm", "unix-shell"});
        needs_image_row();
        const auto os = posix_system();
        if (os.empty()) skip("a POSIX host");
        const auto tmp = s::case_dir();
        const writable_on_exit guard{tmp};
        const auto sh = s::on_path("sh");
        expect(fatal(sh.has_value())) << "sh is not on PATH";
        s::project app(tmp / "app", {{"mcpp.toml", "[package]\nname    = \"pg\"\nversion = \"0.1.0\"\n"},
                                     {"src/main.cpp", "import std;\nint main(int argc, char** argv) { std::println(\"pg {} args, first [{}]\", argc, argc > 1 ? argv[1] : \"\"); }\n"}});
        const auto pair = std::format("{}-{}", os, host_arch);
        // A
        auto r = app.mcpp({"pack", "--target", "openkal", "--pkg-format", "polyglot", "-o", "out/"});
        expect(fatal(e2e::succeeded(r))) << "A: pack: " << s::text(r);
        expect(fatal(names_in(app.at("out")) == std::vector<std::string>{"pg-0.1.0.exe"})) << "A: the files are not one pg-0.1.0.exe: " << s::text(r);
        const auto f = app.at("out/pg-0.1.0.exe");
        const auto bytes = s::read_file(f);
        expect(fatal(bytes.starts_with("MZqFpD='"))) << "A: the file does not begin with the DOS header's script";
        const auto lfanew = bytes.size() > 64 ? std::uint32_t(static_cast<unsigned char>(bytes[60])) | std::uint32_t(static_cast<unsigned char>(bytes[61])) << 8 | std::uint32_t(static_cast<unsigned char>(bytes[62])) << 16 | std::uint32_t(static_cast<unsigned char>(bytes[63])) << 24 : 0u;
        expect(fatal(lfanew + 2 <= bytes.size() && bytes.substr(lfanew, 2) == "PE")) << "A: no PE header at e_lfanew (" << lfanew << ")";
        expect(fatal(s::has_line(s::text(r), "Runs on .*" + pair))) << "A: Runs on does not name " << pair << ": " << s::text(r);

        // B: the cache of an empty home; from here every command runs with that HOME and no
        // XDG_CACHE_HOME, as the script's `export HOME; unset XDG_CACHE_HOME`.
        const auto home = tmp / "home";
        fs::create_directories(home);
        const std::vector<std::pair<std::string, std::optional<std::string>>> env{{"HOME", home.string()}, {"XDG_CACHE_HOME", std::nullopt}};
        const s::options o{.env = env};
        const auto cache = os == "macos" ? home / "Library/Caches/mcpp/single-file" : home / ".cache/mcpp/single-file";
        const auto entries = [](const fs::path& root, bool files_only) {
            std::size_t n = files_only ? 0 : 1;  // `find <root> | wc -l` counts the root
            std::error_code ec;
            if (fs::is_directory(root, ec))
                for (const auto& e : fs::recursive_directory_iterator(root))
                    if (!files_only || e.is_regular_file()) ++n;
            return n;
        };
        auto ran = s::run(*sh, {f.string(), "first"}, {}, env);
        expect(fatal(e2e::succeeded(ran))) << "B: the first run: " << s::text(ran);
        expect(fatal(e2e::contains(s::text(ran), "pg 2 args, first [first]"))) << "B: the first run's output: " << s::text(ran);
        expect(fatal(entries(cache, true) == 2u)) << "B: the first run placed " << entries(cache, true) << " files, expected the program and .mcpp-complete";
        expect(fatal(!s::glob(cache, "*/.mcpp-complete").empty())) << "B: no .mcpp-complete";
        ran = s::run(*sh, {f.string(), "second"}, {}, env);
        expect(fatal(e2e::succeeded(ran))) << "B: the second run: " << s::text(ran);
        expect(fatal(e2e::contains(s::text(ran), "first [second]"))) << "B: the second run's output: " << s::text(ran);
        const auto n2 = entries(cache, false);
        auto bash = e2e::run({.argv = {"bash", "-c", std::format("'{}' viabash", f.string())}, .env = env});
        expect(fatal(e2e::succeeded(bash))) << "B: bash running the file: " << s::text(bash);
        expect(fatal(e2e::contains(s::text(bash), "first [viabash]"))) << "B: bash's run: " << s::text(bash);
        expect(fatal(entries(cache, false) == n2)) << "B: a later run placed more files";

        // C: one byte of this machine's program changed; it lies at `a=` of its line in the
        // script at the end (`tail -c 4096 | grep -a "k=<pair> " | sed 's/.* a=\([0-9]*\) .*/\1/'`).
        const auto last = bytes.substr(bytes.size() > 4096 ? bytes.size() - 4096 : 0);
        std::smatch m;
        const std::regex line{std::format(R"(.* a=([0-9]*) .*)")};
        std::string at;
        std::istringstream lines{last};
        for (std::string l; std::getline(lines, l);)
            if (l.contains(std::format("k={} ", pair)) && std::regex_match(l, m, line)) { at = m[1]; break; }
        expect(fatal(!at.empty())) << "C: no line for " << pair << " in the script";
        const auto pos = std::stoull(at) + 4096;
        expect(fatal(pos < bytes.size())) << "C: the program's offset is past the file";
        auto damaged = bytes;
        damaged[pos] = static_cast<char>((static_cast<unsigned char>(damaged[pos]) + 1) % 256);
        const auto d = tmp / "damaged.exe";
        std::ofstream(d, std::ios::binary) << damaged;
        ran = s::run(*sh, {d.string(), "x"}, {}, {{"HOME", (tmp / "home-c").string()}, {"XDG_CACHE_HOME", ""}});
        expect(fatal(ran.started && ran.status == 127)) << "C: a damaged file exited " << ran.status << ", expected 127: " << s::text(ran);
        expect(fatal(e2e::contains(s::text(ran), "does not match its digest"))) << "C: the refusal does not say why: " << s::text(ran);
        expect(fatal(!s::find_file(tmp / "home-c", ".mcpp-complete").has_value())) << "C: a damaged program was placed";

        // D: a cache directory that is a link
        fs::create_directories(tmp / "home-d/elsewhere");
        const auto caches = os == "macos" ? tmp / "home-d/Library/Caches" : tmp / "home-d/.cache";
        fs::create_directories(caches);
        fs::create_directory_symlink(tmp / "home-d/elsewhere", caches / "mcpp");
        ran = s::run(*sh, {f.string(), "x"}, {}, {{"HOME", (tmp / "home-d").string()}, {"XDG_CACHE_HOME", std::nullopt}});
        expect(fatal(ran.started && ran.status == 127)) << "D: a linked cache directory exited " << ran.status << ", expected 127: " << s::text(ran);
        expect(fatal(e2e::contains(s::text(ran), "is not a directory of this user's"))) << "D: the refusal does not say why: " << s::text(ran);

        // E
        r = app.mcpp({"pack", "--target", "openkal", "--pkg-format", "polyglot", "--systems", os, "-o", "plain/"}, o);
        expect(fatal(e2e::succeeded(r))) << "E: pack: " << s::text(r);
        expect(fatal(fs::is_regular_file(app.at("plain/pg-0.1.0")))) << "E: no pg-0.1.0 (without .exe): " << s::text(r);
        expect(fatal(s::read_file(app.at("plain/pg-0.1.0")).starts_with("#!/bin/sh"))) << "E: not a script";
        ran = s::run(app.at("plain/pg-0.1.0"), {"direct"}, {}, env);
        expect(fatal(e2e::succeeded(ran))) << "E: executed directly: " << s::text(ran);
        expect(fatal(e2e::contains(s::text(ran), "first [direct]"))) << "E: the output: " << s::text(ran);
        // F
        r = app.mcpp({"run", "--target", std::string(host_arch) + "-openkal", "--pkg-format", "polyglot", "--", "viarun"}, o);
        expect(fatal(e2e::succeeded(r))) << "F: mcpp run: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "first [viarun]"))) << "F: the program's output: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "Running `sh "))) << "F: not started through sh: " << s::text(r);
        // G
        r = app.mcpp({"pack", "--target", "openkal", "--pkg-format", "tar,polyglot", "-o", "g/"}, o);
        expect(fatal(!e2e::succeeded(r))) << "G: a list with polyglot was accepted: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "polyglot is not one"))) << "G: the list's refusal: " << s::text(r);
        r = app.mcpp({"pack", "--pkg-format", "polyglot"}, o);
        expect(fatal(!e2e::succeeded(r))) << "G: a native target was accepted: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "is a program of one system"))) << "G: the native refusal: " << s::text(r);
        expect(fatal(s::lacks(s::text(r), "Compiling"))) << "G: the native refusal came after a build: " << s::text(r);
        r = app.mcpp({"run", "--pkg-format", "tar"}, o);
        expect(fatal(!e2e::succeeded(r))) << "G: mcpp run --pkg-format tar was accepted: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "runs a program, and tar is a container"))) << "G: run's refusal: " << s::text(r);
        r = app.mcpp({"emit", "xpkg", "--package", f.string(), "--url", "https://example.invalid"}, o);
        expect(fatal(!e2e::succeeded(r))) << "G: emit xpkg accepted the file: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "is a polyglot file"))) << "G: emit xpkg does not name the polyglot file: " << s::text(r);
        // H
        if (const auto wine = s::on_path("wine"); wine && os == "linux") {
            auto w = e2e::run({.argv = {wine->string(), f.string(), "onwine"}, .env = {{"WINEDEBUG", "-all"}, {"XDG_CACHE_HOME", std::nullopt}}, .timeout = std::chrono::seconds{300}});
            expect(fatal(e2e::succeeded(w))) << "H: wine: " << s::text(w);
            expect(fatal(e2e::contains(s::text(w), "first [onwine]"))) << "H: the Windows program's output: " << s::text(w);
        }
    };

    "995 the windows launcher builds over any platform release"_test[tag{"requires:llvm"}] = [] {
        // A project selecting openkal-platform 0.19.0 packs self-contained for linux and
        // windows: the tree holds bin/<name>.exe and the Windows runner, and the pack names the
        // selected platform release.
        s::needs({"llvm"});
        if (!s::on_linux() || host_arch != "x86_64") skip("the Windows launcher is built here on Linux x86_64");
        s::project app({{"mcpp.toml", "[package]\nname    = \"wl\"\nversion = \"0.1.0\"\n\n[target.'cfg(os = \"openkal\")']\nplatform = \"mcpplibs.openkal-platform@0.19.0\"\n"},
                        {"src/main.cpp", "import std;\nint main(int argc, char**) { std::println(\"wl ok: {} args\", argc); }\n"}});
        auto r = app.mcpp({"pack", "--target", "x86_64-openkal", "--mode", "self-contained", "--systems", "linux,windows", "--pkg-format", "dir", "-o", "t"});
        expect(fatal(e2e::succeeded(r))) << "A: pack: " << s::text(r);
        expect(fatal(fs::is_regular_file(app.at("target/dist/t/bin/wl.exe")))) << "A: no bin/wl.exe: " << s::text(r);
        expect(fatal(fs::is_regular_file(app.at("target/dist/t/libexec/okrun/windows-x86_64/okrun.exe")))) << "A: no Windows runner: " << s::text(r);
        expect(fatal(e2e::contains(s::text(r), "openkal-platform 0.19.0"))) << "B: the selected platform is not the image's: " << s::text(r);
    };

    "1078 systems all carries the runners the platform declares"_test[tag{"requires:llvm"}][tag{"requires:unix-shell"}] = [] {
        // Without `--systems` a self-contained pack carries this machine's system's runner for
        // each leg and no other system's; `--systems all` carries the runners the platform's
        // data names, FreeBSD's among them, and Windows' or says it has none.
        s::needs({"llvm", "unix-shell"});
        needs_image_row();
        if (!s::on_linux()) skip("a Linux host");
        auto app = s::project::scaffold("app");
        // A `dir` pack's tree lands under target/dist whatever `-o` says: it is located (the
        // directory newer than mcpp.toml), not assumed.
        auto r = app.mcpp({"pack", "--target", "openkal", "--mode", "self-contained", "--pkg-format", "dir"});
        expect(fatal(e2e::succeeded(r))) << "the default pack did not build: " << s::text(r);
        std::optional<fs::path> one;
        for (const auto& e : fs::directory_iterator(app.at("target/dist")))
            if (e.is_directory() && fs::last_write_time(e.path()) > fs::last_write_time(app.at("mcpp.toml"))) { one = e.path(); break; }
        expect(fatal(one.has_value())) << "no pack tree under target/dist: " << s::text(r);
        expect(fatal(fs::is_directory(*one / std::format("libexec/okrun/linux-{}", host_arch)))) << "the default pack carries no runner of its own system: " << s::text(r);
        for (const auto& n : names_in(*one / "libexec/okrun"))
            expect(fatal(n.starts_with("linux-"))) << "the default pack carries a system other than this machine's: " << n;
        // The same tree name replaces the default's, so dist is cleared first.
        fs::remove_all(app.at("target/dist"));
        r = app.mcpp({"pack", "--target", "openkal", "--mode", "self-contained", "--systems", "all", "--pkg-format", "dir"});
        expect(fatal(e2e::succeeded(r))) << "--systems all did not pack: " << s::text(r);
        std::optional<fs::path> all;
        for (const auto& e : fs::directory_iterator(app.at("target/dist")))
            if (e.is_directory()) { all = e.path(); break; }
        expect(fatal(all.has_value())) << "no --systems all tree under target/dist: " << s::text(r);
        for (std::string_view p : {"linux-x86_64", "linux-aarch64", "freebsd-x86_64", "freebsd-aarch64"})
            expect(fatal(fs::is_directory(*all / "libexec/okrun" / p))) << "no runner carried for " << p << ": " << s::text(r);
        if (!fs::is_directory(*all / "libexec/okrun/windows-x86_64"))
            expect(fatal(e2e::contains(s::text(r), "no runner for windows-x86_64"))) << "the Windows runner is neither carried nor said: " << s::text(r);
    };
}};

}  // namespace
