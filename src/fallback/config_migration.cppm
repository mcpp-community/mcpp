// mcpp.fallback.config_migration — legacy index name migration.
//
// Older mcpp sandboxes used "mcpp-index" as the default index name.
// These helpers rename it to "mcpplibs" in config.toml and .xlings.json
// so xlings config/list output matches mcpp's default namespace.

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

    return write_text_if_changed(path, original, updated);
}

} // namespace mcpp::fallback
