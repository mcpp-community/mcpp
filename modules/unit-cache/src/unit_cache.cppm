// mcpp.build.unit_cache -- a compile's outputs addressed by what it read (design 2026-11-20 §5.3).
//
// A FRESH CHECKOUT COMPILED EVERY UNIT AGAIN. ninja decides by modification time, and a checkout
// writes every file new: CI rebuilt mcpp's 830 edges in each job (§3.5), and a toolchain upgrade
// that moved the fingerprint directory compiled a project again with nothing changed. The key here
// is the content of what a compile reads, so a unit whose inputs hold the bytes they held is
// placed, not compiled.
//
// TWO LEVELS. What a compile reads is known only after it ran (its depfile), so the first key
// takes what is known before: the command, the directory it runs in, the source's content, the
// response files' and the module mapper's. It names the manifests earlier compiles of that key
// recorded, each a list of the files the compile read with their contents' digests; the first
// whose files all hold those contents names the outputs.
//
// WHAT IS NOT LOOKED AT, AND WHY THAT IS SAFE. A file under an immutable root (a store of
// versioned payloads: the toolchain, the packages of the index) does not change in place; its
// path is its identity, and a different version is another path in the command. The compiler must
// be under one: its system headers do not appear in a `-MMD` depfile, and only a toolchain whose
// files never change makes their absence harmless. A `-isystem` directory outside them is the same
// absence, and refuses the unit.
export module mcpp.build.unit_cache;

import std;
import mcpp.toolchain.fingerprint;

export namespace mcpp::build::unit_cache {

// The cache's directory, and the roots whose files never change in place, joined by the system's
// path list separator: what the build states to the compiler executor.
inline constexpr std::string_view kDirEnv   = "MCPP_UNIT_CACHE";
inline constexpr std::string_view kRootsEnv = "MCPP_IMMUTABLE_ROOTS";
// A build directory's memory of file digests, by path, size and modification time: a header is
// read once per build, not once per unit that includes it.
inline constexpr std::string_view kDigestsFile = ".mcpp-digests";

// One compile, as the executor runs it: the compiler's command, the directory it runs in, and the
// outputs the executor publishes.
struct Unit {
    std::vector<std::string> argv;
    std::filesystem::path    cwd;
    std::filesystem::path    object;
    std::filesystem::path    bmi;   // empty: the unit provides no module
};

// What the command names, read from its words: the source (`-c <file>`), the depfile (`-MF`), the
// module mapper (`-fmodule-mapper=`), the response files (`@file`), the directories searched as
// the system's (`-isystem`, `-idirafter`), and whether the debug information is split.
struct Shape {
    std::filesystem::path              source, depfile, mapper;
    std::vector<std::filesystem::path> responses, systemDirs;
    bool                               splitDebug = false;
};
std::optional<Shape> shape_of(const std::vector<std::string>& argv);

// The prerequisites of every rule of a Makefile-syntax depfile, each once, unescaped.
std::vector<std::string> depfile_prerequisites(std::string_view text);

class Cache {
public:
    Cache(std::filesystem::path dir, std::vector<std::filesystem::path> immutableRoots,
          std::filesystem::path buildDir);
    // Why `u` is not cached, or empty when it is.
    std::string refusal(const Unit& u) const;
    // Places the outputs of an earlier compile that read what this one would: true when it did.
    bool serve(const Unit& u);
    // Records what a compile that succeeded read, and keeps its outputs.
    void keep(const Unit& u);

private:
    std::string first_key(const Unit& u, const Shape& s);
    std::string digest(const std::filesystem::path& file);
    bool immutable(const std::filesystem::path& file) const;
    std::filesystem::path                                    dir_;
    std::vector<std::filesystem::path>                       roots_;
    std::filesystem::path                                    buildDir_;
    std::map<std::string, std::string, std::less<>>          memo_;   // "path\tsize\tmtime" -> digest
    bool                                                     memoLoaded_ = false;
};

}  // namespace mcpp::build::unit_cache

namespace mcpp::build::unit_cache {

namespace fs = std::filesystem;

namespace {

std::string read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

// Written beside `to` and renamed over it, so a reader never sees part of it.
bool place(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    if (to.has_parent_path()) fs::create_directories(to.parent_path(), ec);
    auto tmp = to;
    tmp += std::format(".mcpp-unit-{:x}", std::random_device{}());
    fs::copy_file(from, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) return false;
    fs::rename(tmp, to, ec);
    if (ec) fs::remove(tmp, ec);
    return !ec;
}

std::string normal(const fs::path& base, const fs::path& p) {
    return (p.is_absolute() ? p : base / p).lexically_normal().generic_string();
}

}  // namespace

std::optional<Shape> shape_of(const std::vector<std::string>& argv) {
    Shape s;
    for (std::size_t i = 1; i < argv.size(); ++i) {
        const std::string_view w = argv[i];
        auto next = [&]() -> std::string_view { return i + 1 < argv.size() ? std::string_view(argv[++i]) : std::string_view{}; };
        if (w == "-c") s.source = next();
        else if (w == "-MF") s.depfile = next();
        else if (w.starts_with("-fmodule-mapper=")) s.mapper = w.substr(16);
        else if (w.starts_with("@") && w.size() > 1) s.responses.emplace_back(w.substr(1));
        else if (w == "-isystem" || w == "-idirafter") s.systemDirs.emplace_back(next());
        else if ((w.starts_with("-isystem") || w.starts_with("-idirafter")) && w.size() > 9)
            s.systemDirs.emplace_back(w.starts_with("-isystem") ? w.substr(8) : w.substr(10));
        else if (w == "-gsplit-dwarf") s.splitDebug = true;
    }
    if (s.source.empty() || s.depfile.empty()) return std::nullopt;
    // A module mapper that is a program, or a server, answers what a file cannot record.
    if (!s.mapper.empty() && (s.mapper.native().find('|') != fs::path::string_type::npos
                              || s.mapper.native().find('=') != fs::path::string_type::npos
                              || s.mapper.native().find(':') == 0))
        return std::nullopt;
    return s;
}

std::vector<std::string> depfile_prerequisites(std::string_view text) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    std::string word;
    bool afterColon = false;
    auto end_word = [&] {
        if (afterColon && !word.empty() && seen.insert(word).second) out.push_back(word);
        word.clear();
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\\' && i + 1 < text.size()) {
            const char n = text[i + 1];
            if (n == '\n') { end_word(); ++i; continue; }
            if (n == '\r' && i + 2 < text.size() && text[i + 2] == '\n') { end_word(); i += 2; continue; }
            if (n == ' ' || n == '#' || n == '\\') { word += n; ++i; continue; }
        }
        if (c == '$' && i + 1 < text.size() && text[i + 1] == '$') { word += '$'; ++i; continue; }
        if (c == '\n') { end_word(); afterColon = false; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { end_word(); continue; }
        // The colon that ends a rule's targets: followed by space or the line's end, so a
        // drive letter (`C:/x`) stays in its word.
        if (c == ':' && !afterColon && (i + 1 >= text.size() || text[i + 1] == ' ' || text[i + 1] == '\n'
                                        || text[i + 1] == '\r' || text[i + 1] == '\t')) {
            word.clear();
            afterColon = true;
            continue;
        }
        word += c;
    }
    end_word();
    return out;
}

Cache::Cache(fs::path dir, std::vector<fs::path> immutableRoots, fs::path buildDir)
    : dir_(std::move(dir)), roots_(std::move(immutableRoots)), buildDir_(std::move(buildDir)) {
    for (auto& r : roots_) r = r.lexically_normal();
}

bool Cache::immutable(const fs::path& file) const {
    const auto f = file.lexically_normal().generic_string();
    for (const auto& r : roots_) {
        const auto root = r.generic_string();
        if (!root.empty() && f.starts_with(root) && (f.size() == root.size() || f[root.size()] == '/'
                                                     || root.ends_with('/')))
            return true;
    }
    return false;
}

std::string Cache::digest(const fs::path& file) {
    if (!memoLoaded_) {
        memoLoaded_ = true;
        std::istringstream in(read_all(buildDir_ / kDigestsFile));
        for (std::string line; std::getline(in, line);)
            if (const auto tab = line.rfind('\t'); tab != std::string::npos)
                memo_.emplace(line.substr(0, tab), line.substr(tab + 1));
    }
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (ec) return {};
    const auto mtime = fs::last_write_time(file, ec).time_since_epoch().count();
    if (ec) return {};
    const auto key = std::format("{}\t{}\t{}", file.generic_string(), size, mtime);
    if (auto it = memo_.find(key); it != memo_.end()) return it->second;
    auto d = mcpp::toolchain::hash_file(file);
    memo_.emplace(key, d);
    // A FILE WRITTEN IN THE LAST TWO SECONDS IS NOT REMEMBERED for other compiles: a second write
    // of the same size within one tick of the file system's clock keeps the time, and the memory
    // would then hold the first content's digest (git's "racily clean" entries).
    if (fs::file_time_type::clock::now() - fs::last_write_time(file, ec) < std::chrono::seconds{2} || ec) return d;
    // One line, one append: the units of a build write it concurrently.
    std::ofstream(buildDir_ / kDigestsFile, std::ios::binary | std::ios::app) << key + "\t" + d + "\n";
    return d;
}

std::string Cache::refusal(const Unit& u) const {
    if (u.argv.empty()) return "no command";
    const auto shape = shape_of(u.argv);
    if (!shape) return "the command names no source and depfile";
    if (!immutable(fs::path(u.argv.front())))
        return "the compiler is not in a store whose files never change";
    for (const auto& d : shape->systemDirs)
        if (!immutable(d.is_absolute() ? d : u.cwd / d))
            return std::format("a system include directory, {}, is not in a store", d.generic_string());
    return {};
}

std::string Cache::first_key(const Unit& u, const Shape& s) {
    std::string text = "unit-cache-v1\n";
    text += "cwd=" + u.cwd.lexically_normal().generic_string() + "\n";
    for (const auto& w : u.argv) {
        text += w;
        text += '\0';
    }
    text += "\nsource=" + digest(s.source.is_absolute() ? s.source : u.cwd / s.source) + "\n";
    for (const auto& r : s.responses) text += "response=" + digest(r.is_absolute() ? r : u.cwd / r) + "\n";
    if (!s.mapper.empty()) text += "mapper=" + digest(s.mapper.is_absolute() ? s.mapper : u.cwd / s.mapper) + "\n";
    return mcpp::toolchain::hash_string(text) + mcpp::toolchain::hash_string(text + "\x01");
}

bool Cache::serve(const Unit& u) {
    if (!refusal(u).empty()) return false;
    const auto shape = shape_of(u.argv);
    const auto key = first_key(u, *shape);
    const auto entry = dir_ / key.substr(0, 2) / key;
    std::error_code ec;
    if (!fs::is_directory(entry, ec)) return false;
    for (const auto& m : fs::directory_iterator(entry, ec)) {
        if (!m.is_directory() || m.path().filename().string().starts_with(".")) continue;
        std::istringstream in(read_all(m.path() / "manifest.tsv"));
        bool holds = true;
        bool any = false;
        for (std::string line; holds && std::getline(in, line);) {
            const auto tab = line.find('\t');
            if (tab == std::string::npos) { holds = false; break; }
            any = true;
            const fs::path file = line.substr(0, tab);
            const auto want = line.substr(tab + 1);
            if (want == "=") continue;   // immutable: its path is its identity
            holds = digest(file) == want;
        }
        if (!holds || !any) continue;
        const auto out = m.path();
        // Each output, unless it already holds the bytes (so ninja's restat sees no change).
        auto put = [&](const fs::path& from, const fs::path& to) {
            if (to.empty() || !fs::exists(from, ec)) return true;
            const auto target = to.is_absolute() ? to : u.cwd / to;
            if (fs::exists(target, ec) && mcpp::toolchain::hash_file(target) == mcpp::toolchain::hash_file(from))
                return true;
            return place(from, target);
        };
        if (!put(out / "object", u.object) || !put(out / "bmi", u.bmi) || !put(out / "depfile", shape->depfile))
            return false;
        if (shape->splitDebug) {
            auto dwo = u.object;
            if (!put(out / "dwo", dwo.replace_extension(".dwo"))) return false;
        }
        // Its last use, which `mcpp cache gc` collects by.
        fs::last_write_time(out / "manifest.tsv", fs::file_time_type::clock::now(), ec);
        return true;
    }
    return false;
}

void Cache::keep(const Unit& u) {
    if (!refusal(u).empty()) return;
    const auto shape = shape_of(u.argv);
    const auto depfile = shape->depfile.is_absolute() ? shape->depfile : u.cwd / shape->depfile;
    const auto text = read_all(depfile);
    if (text.empty()) return;
    const auto object = u.object.is_absolute() ? u.object : u.cwd / u.object;
    const auto bmi = u.bmi.empty() ? fs::path{} : (u.bmi.is_absolute() ? u.bmi : u.cwd / u.bmi);
    std::string manifest;
    for (const auto& p : depfile_prerequisites(text)) {
        // GCC names a module a unit imports or provides (`x.c++-module`) beside the files.
        if (p.ends_with(".c++-module")) continue;
        const auto file = fs::path(normal(u.cwd, p));
        if (file == object.lexically_normal() || (!bmi.empty() && file == bmi.lexically_normal())) continue;
        if (immutable(file)) {
            manifest += file.generic_string() + "\t=\n";
            continue;
        }
        const auto d = digest(file);
        if (d.empty()) return;   // a file it read is gone: nothing to record
        manifest += file.generic_string() + "\t" + d + "\n";
    }
    if (manifest.empty()) return;
    const auto key = first_key(u, *shape);
    const auto entry = dir_ / key.substr(0, 2) / key;
    const auto name = mcpp::toolchain::hash_string(manifest);
    std::error_code ec;
    if (fs::is_directory(entry / name, ec)) return;
    const auto staging = entry / std::format(".staging-{:x}", std::random_device{}());
    fs::create_directories(staging, ec);
    if (ec) return;
    bool ok = static_cast<bool>(std::ofstream(staging / "manifest.tsv", std::ios::binary) << manifest);
    ok = ok && fs::exists(object, ec);
    if (ok) fs::copy_file(object, staging / "object", ec), ok = !ec;
    if (ok && !bmi.empty() && fs::exists(bmi, ec)) fs::copy_file(bmi, staging / "bmi", ec), ok = !ec;
    if (ok) fs::copy_file(depfile, staging / "depfile", ec), ok = !ec;
    if (ok && shape->splitDebug) {
        auto dwo = object;
        dwo.replace_extension(".dwo");
        if (fs::exists(dwo, ec)) fs::copy_file(dwo, staging / "dwo", ec), ok = !ec;
    }
    // Published whole by one rename; a compile of the same key that got there first wins.
    if (ok) fs::rename(staging, entry / name, ec);
    if (!ok || ec) fs::remove_all(staging, ec);
}

}  // namespace mcpp::build::unit_cache
