// mcpp.surface -- see mcpp.toml for what this package is.
export module mcpp.surface;

import std;

export namespace mcpp::surface {

// The kinds, in the order `mcpp emit surface` writes them.
inline constexpr std::string_view kCommand = "command";
inline constexpr std::string_view kFlag    = "flag";
inline constexpr std::string_view kKey     = "key";
inline constexpr std::string_view kReason  = "reason";

// The file an invocation appends the items it used to (SPEC-017 P5.6). Unset, nothing is written.
inline constexpr std::string_view kRecordEnv = "MCPP_SURFACE_RECORD";

struct Item {
    std::string kind;
    std::string name;
    auto operator<=>(const Item&) const = default;
};

// `<kind> <name>\n`: the line of `mcpp emit surface` and of the record alike.
std::string line(const Item& item);

// The items of `text`, one per line; a line without a kind and a name is skipped, since a
// process killed while appending leaves a partial last line.
std::vector<Item> parse(std::string_view text);

// The flag item of `--name` on the command `path` ("" for the top level): `build --target`.
std::string flag_name(std::string_view path, std::string_view option);

// Appends `item` to `file` unless this process appended it there already.
void append_once(const std::filesystem::path& file, const Item& item);

// Appends the item to the file MCPP_SURFACE_RECORD names, once per process.
void exercised(std::string_view kind, std::string_view name);

} // namespace mcpp::surface

namespace mcpp::surface {

std::string line(const Item& item) { return std::format("{} {}\n", item.kind, item.name); }

std::vector<Item> parse(std::string_view text) {
    std::vector<Item> out;
    for (auto part : std::views::split(text, '\n')) {
        std::string_view l(part.begin(), part.end());
        if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
        const auto sp = l.find(' ');
        if (sp == std::string_view::npos || sp == 0 || sp + 1 == l.size()) continue;
        out.push_back({std::string(l.substr(0, sp)), std::string(l.substr(sp + 1))});
    }
    return out;
}

std::string flag_name(std::string_view path, std::string_view option) {
    return path.empty() ? std::format("--{}", option) : std::format("{} --{}", path, option);
}

void append_once(const std::filesystem::path& file, const Item& item) {
    // Sub-builds of one invocation run in the same process; a key read by each of them is one use.
    static std::mutex m;
    static std::set<std::pair<std::filesystem::path, Item>> seen;
    std::lock_guard lock(m);
    if (!seen.emplace(file, item).second) return;
    // Appended, never rewritten: a build program's own mcpp writes to the same file.
    std::ofstream os(file, std::ios::app | std::ios::binary);
    os << line(item);
}

void exercised(std::string_view kind, std::string_view name) {
    static const std::string file = [] {
        const char* v = std::getenv(std::string(kRecordEnv).c_str());
        return std::string(v ? v : "");
    }();
    if (!file.empty()) append_once(file, {std::string(kind), std::string(name)});
}

} // namespace mcpp::surface
