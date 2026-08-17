// mcpp.pack.digest — the content digests a library package records, and the
// consumer re-computes.
//
// Its own module, and a leaf, because BOTH sides need it: `mcpp pack` writes
// the digests and `mcpp build` verifies them. Putting it in the packer would
// make the build depend on the packer, which already depends on the build.
//
// `fnv1a:` and not `sha256:`. Three reasons, in order of weight:
//
//   * it needs no external tool. `mcpp publish` shells out to `sha256sum`,
//     which is not on a Windows host — and a package format that can only be
//     verified on some of the platforms it targets is not a format.
//   * the question is "has this changed since it was packaged", not "can an
//     adversary forge a collision". Transport integrity is the index entry's
//     `sha256` over the whole tarball; this is the second line, for a package
//     that has already been extracted or handed over as a directory.
//   * `mcpp.lock` already records `fnv1a:` digests, so the vocabulary is one
//     a reader of this project has already met.

export module mcpp.pack.digest;

import std;
import mcpp.toolchain.fingerprint;

export namespace mcpp::pack {

// `fnv1a:<16 hex>` over one file's bytes.
std::string file_digest(const std::filesystem::path& p);

// The digest of an ordered interface set: each file's NAME and then its
// content hash, sorted by name.
//
// The name is folded in deliberately. Renaming a published interface unit
// changes what a consumer compiles — the `sources` list, and therefore the
// module the BMI comes from — even when not one byte of any file changed.
std::string interface_set_digest(const std::vector<std::filesystem::path>& files);

} // namespace mcpp::pack

namespace mcpp::pack {

std::string file_digest(const std::filesystem::path& p) {
    return "fnv1a:" + mcpp::toolchain::hash_file(p);
}

std::string interface_set_digest(const std::vector<std::filesystem::path>& files) {
    std::vector<std::pair<std::string, std::filesystem::path>> byName;
    for (auto const& f : files) byName.emplace_back(f.filename().string(), f);
    std::ranges::sort(byName, {}, &std::pair<std::string, std::filesystem::path>::first);

    std::string acc;
    for (auto const& [name, path] : byName) {
        acc += name;
        acc += '\x1f';
        acc += mcpp::toolchain::hash_file(path);
        acc += '\x1e';
    }
    return "fnv1a:" + mcpp::toolchain::hash_string(acc);
}

} // namespace mcpp::pack
