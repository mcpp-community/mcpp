// mcpp.provider.roots — the payloads mcpp holds in its home's store, as roots
// (xlings' store-roots specification; design 2026-11-22 §3.11).
//
// mcpp wires store paths into its builds, and the provider's collection
// deletes a payload nothing references. A root is the reference mcpp states:
// a link `<roots>/<store-name>@<version>` to the payload, in the directory the
// provider gives mcpp. A build registers a root for each payload of the store
// its graph names, by replacing the link, so that the link's own time is the
// payload's last use; `mcpp cache gc --older-than` removes the roots unused for
// longer, and the provider's collection does the rest. The collection has one
// implementation, the provider's; when a root goes is mcpp's.

export module mcpp.provider.roots;

import std;
import mcpp.provider;
import mcpp.provider.select;
import mcpp.diag;
import mcpplibs.os.fs;

export namespace mcpp::provider::roots {

// The owner name mcpp's roots are kept under.
inline constexpr std::string_view kOwner = "mcpp";

// The home store's payloads that `text` names (a build graph's text, its
// escapes undone), each once, as `<store-name>/<version>`.
std::vector<std::string> named_in(std::string_view text);

// Hold the payloads of the build in `outputDir`: with `graphText`, the ones
// it names, recorded in `roots.list` beside it; without, the ones the record
// names (a build that reuses its graph). Each root is registered or renewed.
// A root that cannot be made is reported once (`store/roots`): the build goes
// on, and the provider's collection may delete what it uses.
void hold(const std::filesystem::path& outputDir, std::optional<std::string_view> graphText = {});

// Register or renew a root for each `<store-name>/<version>`, or for each
// payload directory of the home store.
void use(const std::vector<std::string>& payloads);
void use_dirs(const std::vector<std::filesystem::path>& payloadDirs);

// THE HOME'S FORMAT STEP: a home that has no roots yet gets one for every
// payload its store holds, dated now, before its first collection. Once per
// home, and idempotent: the roots directory is published whole. Whether the
// home has its roots directory afterwards (a store that does not exist needs
// none, and answers true).
bool seed();

struct Root {
    std::string name;            // `<store-name>@<version>`
    std::int64_t lastUse = 0;    // seconds since the epoch; 0: unknown
};

// The roots last used more than `olderThan` seconds ago, removed unless
// `dryRun`.
std::vector<Root> expire(std::int64_t olderThan, bool dryRun);

} // namespace mcpp::provider::roots

namespace mcpp::provider::roots {

namespace fs = std::filesystem;

namespace {

std::optional<fs::path> store() {
    auto s = mcpp::provider::home_store();
    if (!s || s->empty()) return std::nullopt;
    return s;
}

fs::path directory(const fs::path& store) { return mcpp::provider::layout().roots_of(store, kOwner); }

// A store name and a version are one path component each, of these bytes.
bool component_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_' || c == '+'
           || c == '@';
}

std::string unescape_ninja(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '$' && i + 1 < text.size()
            && (text[i + 1] == ':' || text[i + 1] == ' ' || text[i + 1] == '$')) {
            out.push_back(text[++i]);
            continue;
        }
        out.push_back(text[i]);
    }
    return out;
}

// `<store-name>/<version>` at `at`, or empty.
std::string payload_at(std::string_view text, std::size_t at) {
    std::string parts[2];
    for (auto& part : parts) {
        while (at < text.size() && component_char(text[at])) part.push_back(text[at++]);
        if (part.empty() || part == "." || part == "..") return {};
        if (&part == &parts[0] && (at >= text.size() || (text[at] != '/' && text[at] != '\\'))) return {};
        ++at;
    }
    return parts[0] + "/" + parts[1];
}

void report_once(std::string_view why) {
    static std::once_flag once;
    std::call_once(once, [&] {
        mcpp::diag::degraded("store/roots", std::format("the store's roots of this build were not registered: {}", why),
                             "the provider's collection (`mcpp cache gc`) may remove a payload this build uses; "
                             "the next build installs it again");
    });
}

std::int64_t seconds_of(std::uint64_t ns) { return static_cast<std::int64_t>(ns / 1'000'000'000ull); }

} // namespace

std::vector<std::string> named_in(std::string_view text) {
    std::vector<std::string> out;
    const auto s = store();
    if (!s) return out;
    const auto plain = unescape_ninja(text);
    std::set<std::string> seen;
    for (const auto& prefix : {s->generic_string() + "/", s->string() + std::string(1, fs::path::preferred_separator)}) {
        for (auto at = plain.find(prefix); at != std::string::npos; at = plain.find(prefix, at + prefix.size())) {
            auto p = payload_at(plain, at + prefix.size());
            if (!p.empty() && seen.insert(p).second) out.push_back(std::move(p));
        }
    }
    return out;
}

void use(const std::vector<std::string>& payloads) {
    const auto s = store();
    if (!s || payloads.empty()) return;
    const auto dir = directory(*s);
    std::error_code ec;
    fs::create_directories(dir, ec);
    for (const auto& p : payloads) {
        const auto slash = p.find('/');
        if (slash == std::string::npos || !fs::is_directory(*s / p, ec)) continue;
        const auto name = p.substr(0, slash) + "@" + p.substr(slash + 1);
        // Relative to the owner's directory, as the specification recommends:
        // it survives a move of the home. Made under a temporary name and
        // renamed over the root, so that the root is never absent and its own
        // time is now.
        const auto target = s->lexically_relative(dir) / p.substr(0, slash) / p.substr(slash + 1);
        const auto staged = dir / std::format(".{}.{:08x}", name, std::random_device{}());
        if (auto made = mcpplibs::os::fs::create_link(target, staged, true); !made) {
            report_once(made.error().message());
            return;
        }
        if (auto moved = mcpplibs::os::fs::rename(staged, dir / name, true); !moved) {
            fs::remove(staged, ec);
            report_once(moved.error().message());
            return;
        }
    }
}

void use_dirs(const std::vector<fs::path>& payloadDirs) {
    const auto s = store();
    if (!s) return;
    std::vector<std::string> payloads;
    for (const auto& d : payloadDirs) {
        const auto rel = d.lexically_normal().lexically_relative(s->lexically_normal());
        if (std::distance(rel.begin(), rel.end()) != 2 || rel.begin()->string() == "..") continue;
        payloads.push_back(rel.begin()->string() + "/" + std::next(rel.begin())->string());
    }
    use(payloads);
}

void hold(const fs::path& outputDir, std::optional<std::string_view> graphText) {
    const auto list = outputDir / "roots.list";
    std::vector<std::string> payloads;
    if (graphText) {
        payloads = named_in(*graphText);
        std::string text;
        for (const auto& p : payloads) text += p + "\n";
        std::error_code ec;
        fs::create_directories(outputDir, ec);
        std::ofstream(list, std::ios::binary | std::ios::trunc) << text;
    } else {
        std::ifstream in(list, std::ios::binary);
        for (std::string line; std::getline(in, line);)
            if (!line.empty()) payloads.push_back(line);
    }
    use(payloads);
}

bool seed() {
    const auto s = store();
    if (!s) return true;
    const auto dir = directory(*s);
    std::error_code ec;
    if (fs::exists(dir, ec) || !fs::is_directory(*s, ec)) return true;
    // Published whole: the payloads' roots in a directory of this process's
    // own, renamed to the owner's directory. Another process that seeds at the
    // same time loses the rename and removes its copy; either copy is complete.
    const auto staged = dir.parent_path() / std::format(".{}.seeding.{:08x}", kOwner, std::random_device{}());
    fs::create_directories(staged, ec);
    if (ec) { report_once(ec.message()); return false; }
    for (const auto& e : mcpp::provider::layout().entries(*s)) {
        const auto rel = e.dir.lexically_relative(*s);
        if (rel.empty() || rel.begin() == rel.end() || std::distance(rel.begin(), rel.end()) != 2) continue;
        const auto storeName = rel.begin()->string(), version = std::next(rel.begin())->string();
        const auto target = s->lexically_relative(dir) / storeName / version;
        if (auto made = mcpplibs::os::fs::create_link(target, staged / (storeName + "@" + version), true); !made) {
            fs::remove_all(staged, ec);
            report_once(made.error().message());
            return false;
        }
    }
    if (!mcpplibs::os::fs::rename(staged, dir, false)) fs::remove_all(staged, ec);
    return fs::is_directory(dir, ec);
}

std::vector<Root> expire(std::int64_t olderThan, bool dryRun) {
    std::vector<Root> out;
    const auto s = store();
    if (!s) return out;
    const auto dir = directory(*s);
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator{}; it.increment(ec)) {
        const auto name = it->path().filename().string();
        // A root is a link (a junction on a Windows volume without symbolic
        // links); a file or a directory here is not one.
        if (name.starts_with(".") || !mcpplibs::os::fs::read_link(it->path())) continue;
        // The link's own time, not its payload's: the time mcpp last used it.
        const auto st = mcpplibs::os::fs::stamp_of(it->path(), false);
        if (!st) continue;
        const auto last = seconds_of(st->modified_ns);
        if (now - last <= olderThan) continue;
        if (!dryRun) {
            std::error_code rec;
            fs::remove(it->path(), rec);
            if (rec) continue;
        }
        out.push_back({name, last});
    }
    std::ranges::sort(out, {}, &Root::name);
    return out;
}

} // namespace mcpp::provider::roots
