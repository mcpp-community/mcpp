// mcpp.libs.path_identity — whether two paths name one object, kept apart from
// the paths a program opens.
//
// A path the user or the environment gives (`MCPP_HOME`, `MCPP_TOOLCHAIN`, an
// argument, the working directory) is opened and handed to a program as it is
// spelled, made absolute and lexically normal and no more. Its canonical form
// is an identity: what two spellings have in common when they name one object.
// The two were one value: the home was resolved to its canonical form and every
// path derived from it was opened under that form. Under a sandbox that grants
// a tree through a drive alias (an AppContainer whose `Z:` names a directory
// whose ancestors it may not open), the canonical form leads through those
// ancestors, and the home could not be opened (mcpp#1024). On Linux the same
// conversion turned a home given through a symbolic link into its target.
//
// Identity is asked here. Resolving it reads every ancestor of the path, which
// a restricted process may not do; the answer is then absent, and the caller
// decides with the spelling and says that it did (`diag::degraded`).
// `.github/tools/check_path_identity.sh` counts the resolutions made anywhere
// else, against a baseline that only falls.
//
// It imports nothing of mcpp.

export module mcpp.libs.path_identity;

import std;

export namespace mcpp::libs::path_identity {

// The spelling a given path is opened under: absolute, lexically normal, every
// link and alias kept.
std::filesystem::path given(const std::filesystem::path& p);

// The object `p` names, whatever its spelling: links resolved, and for a path
// whose last components do not exist yet, the existing part resolved and the
// rest appended. nullopt when it cannot be resolved (an ancestor this process
// may not read).
std::optional<std::filesystem::path> identity(const std::filesystem::path& p);

// Whether `a` and `b` name one object. nullopt when either identity cannot be
// resolved; the caller compares the given spellings then, and says so.
std::optional<bool> same(const std::filesystem::path& a, const std::filesystem::path& b);

} // namespace mcpp::libs::path_identity

namespace mcpp::libs::path_identity {

std::filesystem::path given(const std::filesystem::path& p) {
    std::error_code ec;
    auto abs = std::filesystem::absolute(p, ec);
    return (ec ? p : abs).lexically_normal();
}

std::optional<std::filesystem::path> identity(const std::filesystem::path& p) {
    std::error_code ec;
    auto resolved = std::filesystem::weakly_canonical(given(p), ec);
    if (ec) return std::nullopt;
    return resolved;
}

std::optional<bool> same(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code ec;
    if (std::filesystem::exists(a, ec) && std::filesystem::exists(b, ec)) {
        const bool eq = std::filesystem::equivalent(a, b, ec);
        if (!ec) return eq;
    }
    auto ia = identity(a), ib = identity(b);
    if (!ia || !ib) return std::nullopt;
    return *ia == *ib;
}

} // namespace mcpp::libs::path_identity
