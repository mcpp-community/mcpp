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
    std::string              artifact;  // optional artifact source base (xlings >= 0.4.68, #269)
    std::string              source;    // optional "auto" | "artifact" | "git"

    bool is_local()   const { return !path.empty(); }
    bool is_pinned()  const { return !rev.empty(); }
    // The artifact channel only tracks the latest published pointer; any
    // rev/tag/branch pin (and local path) therefore forces git and the
    // artifact declaration is ignored (with a warning at seed time).
    bool artifact_applicable() const {
        return !artifact.empty() && rev.empty() && tag.empty()
            && branch.empty() && path.empty();
    }
    // R6: `name == "mcpplibs"` alone used to mean "builtin" unconditionally,
    // which was correct while the only way to reach that name was the
    // literal `[indices] mcpplibs = { url = ..., rev = ... }` pin form (still
    // routes through the shared global registry, just pinned to a commit —
    // `path` is never set in that form). Since [indices] now also accepts
    // `default = {...}` / `"" = {...}` as aliases for the SAME map entry
    // (both normalize to the "mcpplibs" name — see toml.cppm's [indices]
    // parse), a `path`-based redirect of the default namespace to a local
    // checkout would otherwise be misidentified as "builtin" too, and get
    // silently skipped by the project-index plumbing (ensure_project_index_dir
    // / prepare.cppm's useProjectEnv) instead of actually redirecting.
    // `path` set is an unambiguous signal that this entry does NOT point at
    // the real upstream builtin registry, regardless of which spelling
    // produced the "mcpplibs" name.
    //
    // Note: this only covers the `path` form. A `default`/`""`-alias entry
    // with `url` (not `path`) would ALSO satisfy `path.empty()` here and be
    // misidentified as builtin, silently no-opping the redirect — so
    // toml.cppm's `[indices]` parser rejects that combination at parse time
    // (loud error) instead of letting it reach this predicate. The literal
    // `[indices] mcpplibs = { url = ..., rev = ... }` pin form is unaffected
    // by that rejection and still resolves `is_builtin() == true` here, as
    // intended.
    bool is_builtin() const { return name == "mcpplibs" && path.empty(); }
};

} // namespace mcpp::pm
