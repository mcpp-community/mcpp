// mcpp.fallback.config_migration — legacy index config migration.
//
// Older mcpp sandboxes used "mcpp-index" as the default index name and the
// pre-migration mcpp-community org URL. These helpers rename the index to
// "mcpplibs", rewrite the org URL (#267), and inject the xlings >= 0.4.68
// per-repo artifact declaration (#269) into config.toml / .xlings.json.
// Deliberately text-based: .xlings.json carries xlings-owned state (subos,
// version bindings) that a regeneration would destroy. This is a LEAF
// module (no mcpp.config import), so the URL literals are repeated here.

export module mcpp.fallback.config_migration;

import std;

export namespace mcpp::fallback {

// Migrate config.toml: rename "mcpp-index" to "mcpplibs".
// Returns true if the file was modified.
bool migrate_config_toml_index_names(const std::filesystem::path& path);

// Migrate .xlings.json: rename "mcpp-index" to "mcpplibs".
// Returns true if the file was modified.
bool migrate_xlings_json_index_names(const std::filesystem::path& path);

} // namespace mcpp::fallback

namespace mcpp::fallback {

namespace {

bool replace_all(std::string& text, std::string_view from, std::string_view to) {
    bool changed = false;
    for (std::size_t pos = 0;
         (pos = text.find(from, pos)) != std::string::npos;) {
        text.replace(pos, from.size(), to);
        pos += to.size();
        changed = true;
    }
    return changed;
}

void write_file(const std::filesystem::path& p, std::string_view content) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream os(p);
    os << content;
}

bool write_text_if_changed(const std::filesystem::path& path,
                           const std::string& original,
                           const std::string& updated) {
    if (updated == original) return false;
    write_file(path, updated);
    return true;
}

} // namespace

bool migrate_config_toml_index_names(const std::filesystem::path& path) {
    std::ifstream is(path);
    if (!is) return false;
    std::stringstream ss;
    ss << is.rdbuf();
    auto original = ss.str();
    auto updated = original;

    replace_all(updated, "default = \"mcpp-index\"", "default = \"mcpplibs\"");
    replace_all(updated, "[index.repos.\"mcpp-index\"]", "[index.repos.\"mcpplibs\"]");
    // Org migration (#267): the index repo moved to the mcpplibs org.
    replace_all(updated, "https://github.com/mcpp-community/mcpp-index.git",
                         "https://github.com/mcpplibs/mcpp-index.git");

    return write_text_if_changed(path, original, updated);
}

bool migrate_xlings_json_index_names(const std::filesystem::path& path) {
    std::ifstream is(path);
    if (!is) return false;
    std::stringstream ss;
    ss << is.rdbuf();
    auto original = ss.str();
    auto updated = original;

    replace_all(updated, "\"name\": \"mcpp-index\"", "\"name\": \"mcpplibs\"");
    replace_all(updated, "\"name\":\"mcpp-index\"", "\"name\":\"mcpplibs\"");
    // Org migration (#267): the index repo moved to the mcpplibs org.
    replace_all(updated, "https://github.com/mcpp-community/mcpp-index.git",
                         "https://github.com/mcpplibs/mcpp-index.git");

    // Artifact injection (#269, xlings >= 0.4.68 per-repo artifact source).
    // Existing installs never re-seed .xlings.json, so this migration is
    // their only channel to the artifact declaration; older xlings safely
    // ignores the key. Idempotency gate: the res base appearing anywhere
    // means a previous run (or a fresh seed) already declared it — a plain
    // replace_all would re-inject on every run. Both spacing variants
    // because the file has two writers (mcpp pretty / xlings compact).
    constexpr std::string_view region =
        "{ \"GLOBAL\": \"https://github.com/xlings-res/mcpp-index\", "
        "\"CN\": \"https://gitcode.com/xlings-res/mcpp-index\" }";
    if (updated.find("xlings-res/mcpp-index") == std::string::npos) {
        const std::string art = std::string(", \"artifact\": ") + std::string(region);
        for (std::string_view urlkv : {
                "\"url\": \"https://github.com/mcpplibs/mcpp-index.git\"",
                "\"url\":\"https://github.com/mcpplibs/mcpp-index.git\"" })
            replace_all(updated, urlkv, std::string(urlkv) + art);
    }

    // The CN half of the default artifact (#648 A6). An existing home keeps the
    // GitHub-only base it was seeded with, and its default entry is not
    // reconciled from configuration, so this is the channel that gives it the
    // GitCode mirror. Only the exact default value is rewritten: a base the user
    // wrote is theirs. Idempotent, because the rewritten value no longer
    // contains the flat spelling. Both spacings, for the two writers.
    if (updated.find("gitcode.com/xlings-res/mcpp-index") == std::string::npos) {
        for (std::string_view flat : {
                "\"artifact\": \"https://github.com/xlings-res/mcpp-index\"",
                "\"artifact\":\"https://github.com/xlings-res/mcpp-index\"" })
            replace_all(updated, flat, std::string("\"artifact\": ") + std::string(region));
    }

    return write_text_if_changed(path, original, updated);
}

} // namespace mcpp::fallback
