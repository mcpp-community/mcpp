// mcpp.notice -- see mcpp.toml for what this package is.
export module mcpp.notice;

import std;

export namespace mcpp::notice {

enum class Policy {
    Always,  // said every time it is reported; one statement per process per text (mcpp.diag)
    Once,    // said once per project and key
};

// A notice kind whose policy is not `Always`. `docs` is where the reader is pointed.
struct Kind {
    std::string_view domain;
    Policy           policy;
    std::string_view docs;
};

// The kinds with a policy of their own. A notice that changes what this build does (a
// degradation, a deprecated reading, an unknown key) is never `Once`: suppressing it would make
// doing less silent.
inline constexpr Kind kKinds[] = {
    {"build-program/includes-header", Policy::Once, "docs/30-build-mcpp.md, \"Headers or modules\""},
    {"build-program/reads-outside",   Policy::Once, "docs/30-build-mcpp.md, \"Headers or modules\""},
    {"toolchain/default-changed",     Policy::Once, "docs/20-toolchains.md, \"Resolution and the lock\""},
    {"host-input",                    Policy::Once, "docs/25-home-indices-and-store.md, \"Host inputs\""},
    {"machine-path-in-manifest",      Policy::Once, "docs/25-home-indices-and-store.md, \"Archive directories\""},
    {"bundle-lacks-target",           Policy::Always, "docs/05-dependencies.md, \"Offline builds and mcpp fetch\""},
};

// The row of `domain`, or none.
const Kind* find(std::string_view domain);

// The policy of `domain`: its row's, `Always` without one.
Policy policy_of(std::string_view domain);

// The file a project's record lives in, under its `target/`.
inline constexpr std::string_view kRecordFile = ".mcpp-notices";

// What a project has been told, and the facts its notices compare against. One line per entry:
// `shown<TAB><key>` and `fact<TAB><name><TAB><value>`.
class Record {
public:
    // The record at `file`; an absent or unreadable file is an empty record.
    static Record load(const std::filesystem::path& file);

    const std::filesystem::path& file() const { return file_; }
    bool shown(std::string_view key) const;
    void mark(std::string key);
    std::optional<std::string> fact(std::string_view name) const;
    void set_fact(std::string name, std::string value);

    // Merges this record into the file as it is on disk now and replaces it (a temporary, then a
    // rename). Two processes that save at once can lose each other's entries; the cost is one
    // notice said once more, so no lock is taken.
    bool save() const;

    // The text of the record, as `save` writes it.
    std::string text() const;
    static Record parse(std::string_view text, std::filesystem::path file = {});

private:
    std::filesystem::path              file_;
    std::set<std::string>              shown_;
    std::map<std::string, std::string> facts_;
};

// The key of a notice: its domain and its subject (a package, a file, a tool).
std::string key(std::string_view domain, std::string_view subject);

// The subjects of one notice as one phrase: `a, b and c`; past `limit`, the first `limit` and
// `and N more`.
std::string subjects(std::span<const std::string> names, std::size_t limit = 5);

} // namespace mcpp::notice

namespace mcpp::notice {

const Kind* find(std::string_view domain) {
    for (auto const& k : kKinds)
        if (k.domain == domain) return &k;
    return nullptr;
}

Policy policy_of(std::string_view domain) {
    const auto* k = find(domain);
    return k ? k->policy : Policy::Always;
}

std::string key(std::string_view domain, std::string_view subject) {
    return std::format("{}\x1f{}", domain, subject);
}

Record Record::parse(std::string_view text, std::filesystem::path file) {
    Record r;
    r.file_ = std::move(file);
    for (auto part : std::views::split(text, '\n')) {
        std::string_view line(part.begin(), part.end());
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const auto t1 = line.find('\t');
        if (t1 == std::string_view::npos) continue;  // a partial last line of a killed writer
        const auto tag = line.substr(0, t1);
        const auto rest = line.substr(t1 + 1);
        if (tag == "shown" && !rest.empty()) {
            r.shown_.insert(std::string(rest));
        } else if (tag == "fact") {
            const auto t2 = rest.find('\t');
            if (t2 == std::string_view::npos || t2 == 0) continue;
            r.facts_[std::string(rest.substr(0, t2))] = std::string(rest.substr(t2 + 1));
        }
    }
    return r;
}

Record Record::load(const std::filesystem::path& file) {
    std::ifstream is(file, std::ios::binary);
    std::string text;
    if (is) text.assign(std::istreambuf_iterator<char>(is), {});
    return parse(text, file);
}

bool Record::shown(std::string_view key) const { return shown_.contains(std::string(key)); }
void Record::mark(std::string key) { shown_.insert(std::move(key)); }

std::optional<std::string> Record::fact(std::string_view name) const {
    if (auto it = facts_.find(std::string(name)); it != facts_.end()) return it->second;
    return std::nullopt;
}

void Record::set_fact(std::string name, std::string value) { facts_[std::move(name)] = std::move(value); }

std::string Record::text() const {
    std::string out;
    for (auto const& k : shown_) out += std::format("shown\t{}\n", k);
    for (auto const& [name, value] : facts_) out += std::format("fact\t{}\t{}\n", name, value);
    return out;
}

bool Record::save() const {
    if (file_.empty()) return false;
    // This process's facts win over the disk's; keys are a union.
    auto merged = load(file_);
    for (auto const& k : shown_) merged.shown_.insert(k);
    for (auto const& [name, value] : facts_) merged.facts_[name] = value;
    std::error_code ec;
    std::filesystem::create_directories(file_.parent_path(), ec);
    auto tmp = file_;
    tmp += std::format(".{:x}.tmp", std::random_device{}());
    {
        std::ofstream os(tmp, std::ios::binary | std::ios::trunc);
        if (!os) return false;
        os << merged.text();
        if (!os) return false;
    }
    std::filesystem::rename(tmp, file_, ec);
    if (ec) std::filesystem::remove(tmp, ec);
    return !ec;
}

std::string subjects(std::span<const std::string> names, std::size_t limit) {
    if (names.empty()) return {};
    const auto shown = std::min(names.size(), std::max<std::size_t>(limit, 1));
    std::string out;
    for (std::size_t i = 0; i < shown; ++i) {
        if (i > 0) out += (i + 1 == shown && shown == names.size()) ? " and " : ", ";
        out += names[i];
    }
    if (shown < names.size()) out += std::format(" and {} more", names.size() - shown);
    return out;
}

} // namespace mcpp::notice
