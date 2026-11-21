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
// Beside a BMI the cache kept or placed: the key of the compile that wrote it. A GCC BMI is not
// the same bytes from one compile of a module to the next (measured on mcpp's own modules:
// mcpp.build.prepare and mcpp.toolchain.triple), so an importer names the BMI it read by its
// producer's key, which equal inputs give equal, and its bytes only where no key is beside it.
inline constexpr std::string_view kProducerSuffix = ".mcpp-unit";

// One compile, as the executor runs it: the compiler's command, the directory it runs in, and the
// outputs the executor publishes.
struct Unit {
    std::vector<std::string> argv;
    std::filesystem::path    cwd;
    std::filesystem::path    object;
    std::filesystem::path    bmi;   // empty: the unit provides no module
    // The files the compile reads, where the executor knows them before the compiler runs (the
    // split schedule: the scan's depfile and the BMIs its imports name); unset: the command's
    // depfile states them once it ran.
    std::optional<std::vector<std::filesystem::path>> reads;
    // Where a hit places the object and the BMI when the compiler would have written them
    // elsewhere than where they are published (a relocated attempt); empty: `object`, `bmi`.
    std::filesystem::path placeObject, placeBmi;
};

// A unit as one text and back, for the executor that decides to the one that compiles.
std::string to_record(const Unit& u);
std::optional<Unit> from_record(std::string_view text);

// The words of a command written as one line, quotes and backslashes read as a POSIX shell reads
// them (a rule's command file).
std::vector<std::string> split_command(std::string_view line);

// The BMIs a scan's P1689 record says the unit imports, by the module mapper's lines
// (`$root <dir>`, then `<module> <path>`), relative to `cwd`.
std::vector<std::filesystem::path> imported_bmis(std::string_view p1689, std::string_view mapper,
                                                 const std::filesystem::path& cwd);

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

// WHAT A RECORD KEEPS OF A FILE IT READ, and the one rule by which it is compared: by size and
// modification time while both agree, by content otherwise. The digest memory of a build
// directory (`Cache`), the files a build program's compile read and the files it declares
// (`rerun_if_changed`) are all compared by this rule.
//
// A FILE WRITTEN IN THE LAST TWO SECONDS keeps no time (`mtime == 0`): a second write of the same
// size within one tick of the file system's clock keeps the time, and a comparison by time would
// then hold the first content (git's "racily clean" entries). Such a stamp is compared by content.
struct Stamp {
    std::uintmax_t size = 0;
    std::int64_t   mtime = 0;
    std::string    digest;
};

// Whether `file` was written too recently for its time to identify its content.
bool racily_new(const std::filesystem::path& file);

// The stamp of `file` now, its digest computed; nullopt when it cannot be read.
std::optional<Stamp> stamp_of(const std::filesystem::path& file);

// Whether `file` holds the content `was` recorded. When it does and its size or time moved,
// `refreshed` (if given) receives the stamp now, for the record to keep.
bool unchanged(const std::filesystem::path& file, const Stamp& was,
               std::optional<Stamp>* refreshed = nullptr);

// Whether `file` has the size and time `was` recorded, reading no content: false when either
// differs or the stamp keeps no time. A reader that cannot refresh the record (the build's fast
// path) asks this, and a false answer sends the build to `unchanged`.
bool same_by_stat(const std::filesystem::path& file, const Stamp& was);

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
    // A compile the cache did not keep: its BMI has no producer key.
    static void forget(const Unit& u);
    // Before a compile whose reads are known: its BMI's producer key, beside the published BMI, so
    // that an importer that starts once the BMI is published reads it (the split schedule
    // publishes a BMI while its object is still being compiled).
    void announce(const Unit& u);

private:
    std::string first_key(const Unit& u, const Shape& s);
    // The manifest of what `u` read, its lines sorted; nullopt when a file it read is gone.
    std::optional<std::string> manifest_of(const Unit& u, const std::vector<std::string>& read);
    std::string digest(const std::filesystem::path& file);
    // What a manifest records of a file it read: its producer's key, else its digest.
    std::string identity(const std::filesystem::path& file);
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
    if (s.source.empty()) return std::nullopt;
    // A module mapper that is a program, or a server, answers what a file cannot record.
    if (!s.mapper.empty() && (s.mapper.native().find('|') != fs::path::string_type::npos
                              || s.mapper.native().find('=') != fs::path::string_type::npos
                              || s.mapper.native().find(':') == 0))
        return std::nullopt;
    return s;
}

bool racily_new(const fs::path& file) {
    std::error_code ec;
    const auto t = fs::last_write_time(file, ec);
    return ec || fs::file_time_type::clock::now() - t < std::chrono::seconds{2};
}

namespace {
// Size and time of `file`, the time 0 while it is racily new; nullopt when it cannot be stat'ed.
std::optional<std::pair<std::uintmax_t, std::int64_t>> stat_of(const fs::path& file) {
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (ec) return std::nullopt;
    const auto t = fs::last_write_time(file, ec);
    if (ec) return std::nullopt;
    return std::pair{size, racily_new(file) ? std::int64_t{0}
                                            : static_cast<std::int64_t>(t.time_since_epoch().count())};
}
}  // namespace

std::optional<Stamp> stamp_of(const fs::path& file) {
    const auto st = stat_of(file);
    if (!st) return std::nullopt;
    auto digest = mcpp::toolchain::hash_file(file);
    if (digest.empty()) return std::nullopt;
    return Stamp{st->first, st->second, std::move(digest)};
}

bool same_by_stat(const fs::path& file, const Stamp& was) {
    if (was.mtime == 0) return false;
    const auto st = stat_of(file);
    return st && st->first == was.size && st->second == was.mtime;
}

bool unchanged(const fs::path& file, const Stamp& was, std::optional<Stamp>* refreshed) {
    if (same_by_stat(file, was)) return true;
    auto now = stamp_of(file);
    if (!now || now->digest != was.digest) return false;
    if (refreshed && (now->size != was.size || now->mtime != was.mtime)) *refreshed = std::move(now);
    return true;
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

std::string to_record(const Unit& u) {
    std::string out;
    auto line = [&](std::string_view key, std::string_view value) {
        out += key;
        out += '\t';
        out += value;
        out += '\n';
    };
    line("cwd", u.cwd.generic_string());
    line("object", u.object.generic_string());
    line("bmi", u.bmi.generic_string());
    line("place_object", u.placeObject.generic_string());
    line("place_bmi", u.placeBmi.generic_string());
    for (const auto& w : u.argv) line("argv", w);
    if (u.reads) {
        line("reads", "");
        for (const auto& r : *u.reads) line("read", r.generic_string());
    }
    return out;
}

std::optional<Unit> from_record(std::string_view text) {
    Unit u;
    while (!text.empty()) {
        const auto nl = text.find('\n');
        const auto line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        const auto tab = line.find('\t');
        if (tab == std::string_view::npos) continue;
        const auto key = line.substr(0, tab);
        const std::string value(line.substr(tab + 1));
        if (key == "cwd") u.cwd = value;
        else if (key == "object") u.object = value;
        else if (key == "bmi") u.bmi = value;
        else if (key == "place_object") u.placeObject = value;
        else if (key == "place_bmi") u.placeBmi = value;
        else if (key == "argv") u.argv.push_back(value);
        else if (key == "reads") u.reads.emplace();
        else if (key == "read" && u.reads) u.reads->emplace_back(value);
    }
    if (u.argv.empty() || u.cwd.empty()) return std::nullopt;
    return u;
}

std::vector<std::string> split_command(std::string_view line) {
    std::vector<std::string> out;
    std::string word;
    bool inWord = false;
    char quote = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quote == '\'') {
            if (c == '\'') quote = 0;
            else word += c;
            continue;
        }
        if (c == '\\' && i + 1 < line.size() && quote != '\'') {
            word += line[++i];
            inWord = true;
            continue;
        }
        if (quote == '"') {
            if (c == '"') quote = 0;
            else word += c;
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            inWord = true;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (inWord) out.push_back(std::move(word));
            word.clear();
            inWord = false;
            continue;
        }
        word += c;
        inWord = true;
    }
    if (inWord) out.push_back(std::move(word));
    return out;
}

std::vector<fs::path> imported_bmis(std::string_view p1689, std::string_view mapper, const fs::path& cwd) {
    std::map<std::string, fs::path, std::less<>> path_of;
    fs::path root;
    std::istringstream in{std::string(mapper)};
    for (std::string line; std::getline(in, line);) {
        const auto sp = line.find(' ');
        if (sp == std::string::npos) continue;
        const auto name = line.substr(0, sp);
        const auto where = line.substr(sp + 1);
        if (name == "$root") root = where;
        else path_of.emplace(name, where);
    }
    std::vector<fs::path> out;
    const auto requires_at = p1689.find("\"requires\"");
    if (requires_at == std::string_view::npos) return out;
    constexpr std::string_view kName = "\"logical-name\"";
    for (auto at = p1689.find(kName, requires_at); at != std::string_view::npos; at = p1689.find(kName, at + 1)) {
        const auto open = p1689.find('"', at + kName.size());
        const auto close = open == std::string_view::npos ? open : p1689.find('"', open + 1);
        if (close == std::string_view::npos) break;
        const auto name = p1689.substr(open + 1, close - open - 1);
        if (auto it = path_of.find(name); it != path_of.end()) {
            const auto p = it->second.is_absolute() ? it->second : root / it->second;
            out.push_back(p.is_absolute() ? p : (cwd / p).lexically_normal());
        }
    }
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
    // Not remembered for other compiles while its time cannot identify its content (`Stamp`).
    if (racily_new(file)) return d;
    // One line, one append: the units of a build write it concurrently.
    std::ofstream(buildDir_ / kDigestsFile, std::ios::binary | std::ios::app) << key + "\t" + d + "\n";
    return d;
}

std::string Cache::identity(const fs::path& file) {
    auto producer = file;
    producer += kProducerSuffix;
    std::error_code ec;
    if (fs::is_regular_file(producer, ec))
        if (auto key = read_all(producer); !key.empty()) return "unit:" + key;
    return digest(file);
}

namespace {
void write_producer(const fs::path& bmi, std::string_view key) {
    if (bmi.empty()) return;
    auto producer = bmi;
    producer += kProducerSuffix;
    std::ofstream(producer, std::ios::binary | std::ios::trunc) << key;
}
}  // namespace

void Cache::forget(const Unit& u) {
    if (u.bmi.empty()) return;
    auto producer = u.bmi.is_absolute() ? u.bmi : u.cwd / u.bmi;
    producer += kProducerSuffix;
    std::error_code ec;
    fs::remove(producer, ec);
}

std::string Cache::refusal(const Unit& u) const {
    if (u.argv.empty()) return "no command";
    const auto shape = shape_of(u.argv);
    if (!shape) return "the command names no source";
    if (shape->depfile.empty() && !u.reads) return "nothing states the files the compile reads";
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
            holds = identity(file) == want;
        }
        if (!holds || !any) continue;
        const auto out = m.path();
        // Each output, unless it already holds the bytes (so ninja's restat sees no change).
        // A place the compiler would have written is written whatever it holds: the executor
        // watches it for the compile's outputs.
        auto put = [&](const fs::path& from, const fs::path& to, bool always) {
            if (to.empty() || !fs::exists(from, ec)) return true;
            const auto target = to.is_absolute() ? to : u.cwd / to;
            if (!always && fs::exists(target, ec)
                && mcpp::toolchain::hash_file(target) == mcpp::toolchain::hash_file(from))
                return true;
            return place(from, target);
        };
        if (!put(out / "object", u.placeObject.empty() ? u.object : u.placeObject, !u.placeObject.empty())
            || !put(out / "bmi", u.placeBmi.empty() ? u.bmi : u.placeBmi, !u.placeBmi.empty())
            || (!shape->depfile.empty() && !put(out / "depfile", shape->depfile, false)))
            return false;
        if (shape->splitDebug) {
            auto dwo = u.placeObject.empty() ? u.object : u.placeObject;
            if (!put(out / "dwo", dwo.replace_extension(".dwo"), !u.placeObject.empty())) return false;
        }
        if (!u.bmi.empty()) write_producer(u.bmi.is_absolute() ? u.bmi : u.cwd / u.bmi, key + "/" + out.filename().string());
        // Its last use, which `mcpp cache gc` collects by.
        fs::last_write_time(out / "manifest.tsv", fs::file_time_type::clock::now(), ec);
        return true;
    }
    return false;
}

std::optional<std::string> Cache::manifest_of(const Unit& u, const std::vector<std::string>& read) {
    const auto object = (u.object.is_absolute() ? u.object : u.cwd / u.object).lexically_normal();
    const auto bmi = u.bmi.empty() ? fs::path{} : (u.bmi.is_absolute() ? u.bmi : u.cwd / u.bmi).lexically_normal();
    // One line per file, sorted: the same files read in another order are the same manifest.
    std::set<std::string> lines;
    for (const auto& p : read) {
        // GCC names a module a unit imports or provides (`x.c++-module`) beside the files.
        if (p.ends_with(".c++-module")) continue;
        const auto file = fs::path(normal(u.cwd, p));
        if (file == object || (!bmi.empty() && file == bmi)) continue;
        if (immutable(file)) {
            lines.insert(file.generic_string() + "\t=\n");
            continue;
        }
        const auto d = identity(file);
        if (d.empty()) return std::nullopt;   // a file it read is gone: nothing to record
        lines.insert(file.generic_string() + "\t" + d + "\n");
    }
    std::string manifest;
    for (const auto& l : lines) manifest += l;
    return manifest;
}

void Cache::announce(const Unit& u) {
    if (u.bmi.empty() || !u.reads || !refusal(u).empty()) return;
    std::vector<std::string> read;
    for (const auto& r : *u.reads) read.push_back(r.generic_string());
    const auto manifest = manifest_of(u, read);
    if (!manifest || manifest->empty()) return forget(u);
    const auto shape = shape_of(u.argv);
    write_producer(u.bmi.is_absolute() ? u.bmi : u.cwd / u.bmi,
                   first_key(u, *shape) + "/" + mcpp::toolchain::hash_string(*manifest));
}

void Cache::keep(const Unit& u) {
    if (!refusal(u).empty()) return forget(u);
    const auto shape = shape_of(u.argv);
    const auto depfile = shape->depfile.empty() ? fs::path{}
                       : shape->depfile.is_absolute() ? shape->depfile : u.cwd / shape->depfile;
    std::vector<std::string> read;
    if (u.reads) {
        for (const auto& r : *u.reads) read.push_back(r.generic_string());
    } else {
        const auto text = read_all(depfile);
        if (text.empty()) return forget(u);
        read = depfile_prerequisites(text);
    }
    // A compile reads its source at least: a unit that names no file read states nothing a hit
    // could be checked against.
    if (read.empty()) return forget(u);
    const auto object = u.object.is_absolute() ? u.object : u.cwd / u.object;
    const auto bmi = u.bmi.empty() ? fs::path{} : (u.bmi.is_absolute() ? u.bmi : u.cwd / u.bmi);
    // A BMI the unit provides: where it is published, else where the compiler wrote it (phase 1
    // of the split schedule may not have published it yet). Neither: not kept, a hit would place none.
    const auto written = u.placeBmi.empty() ? fs::path{} : (u.placeBmi.is_absolute() ? u.placeBmi : u.cwd / u.placeBmi);
    const auto bmiBytes = bmi.empty() ? fs::path{} : fs::exists(bmi) ? bmi : (!written.empty() && fs::exists(written)) ? written : fs::path{};
    if (!bmi.empty() && bmiBytes.empty()) return forget(u);
    const auto manifest = manifest_of(u, read).value_or("");
    if (manifest.empty()) return forget(u);
    const auto key = first_key(u, *shape);
    const auto entry = dir_ / key.substr(0, 2) / key;
    const auto name = mcpp::toolchain::hash_string(manifest);
    std::error_code ec;
    write_producer(bmi, key + "/" + name);
    if (fs::is_directory(entry / name, ec)) return;
    const auto staging = entry / std::format(".staging-{:x}", std::random_device{}());
    fs::create_directories(staging, ec);
    if (ec) return;
    bool ok = static_cast<bool>(std::ofstream(staging / "manifest.tsv", std::ios::binary) << manifest);
    ok = ok && fs::exists(object, ec);
    if (ok) fs::copy_file(object, staging / "object", ec), ok = !ec;
    if (ok && !bmiBytes.empty()) fs::copy_file(bmiBytes, staging / "bmi", ec), ok = !ec;
    if (ok && !depfile.empty()) fs::copy_file(depfile, staging / "depfile", ec), ok = !ec;
    if (ok && shape->splitDebug) {
        auto dwo = object;
        dwo.replace_extension(".dwo");
        if (fs::exists(dwo, ec)) fs::copy_file(dwo, staging / "dwo", ec), ok = !ec;
    }
    // Published whole by one rename; a compile of the same key that got there first wins.
    if (ok) fs::rename(staging, entry / name, ec);
    if (!ok || ec) fs::remove_all(staging, ec);
    if (!ok) forget(u);
}

}  // namespace mcpp::build::unit_cache
