// mcpp.pm.index_spec — package-index repository configuration.
//
// `[indices]` in mcpp.toml and config.toml maps index names to their
// location (git URL or local path) with optional version pinning.
// See `.agents/docs/2026-05-16-indices-enhancement-design.md` for the
// full design.

export module mcpp.pm.index_spec;

import std;

export namespace mcpp::pm {

struct IndexSpec {
    std::string              name;      // index name ([indices] key)
    std::string              url;       // git URL (short form fills this directly)
    std::string              rev;       // commit sha (strongest lock)
    std::string              tag;       // git tag
    std::string              branch;    // git branch
    std::filesystem::path    path;      // local path (takes priority over url)

    bool is_local()   const { return !path.empty(); }
    bool is_pinned()  const { return !rev.empty(); }
    bool is_builtin() const { return name == "mcpplibs"; }
};

} // namespace mcpp::pm
