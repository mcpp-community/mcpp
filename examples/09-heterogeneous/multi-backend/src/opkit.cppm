// The seam, as a module.
//
// The backends underneath are C: an island is compiled by a compiler mcpp did
// not choose, so the boundary carries raw pointers and a count and nothing that
// depends on a C++ ABI. This module is where that becomes C++ again, and it is
// the only file a consumer imports -- which is what lets a backend be added,
// removed or reordered without any importer changing.
module;
#include "opkit/opkit.h"
export module opkit;
import std;

export namespace opkit {

// The operator. Which backend serves it is decided inside, at run time, among
// those this build compiled in.
std::optional<std::vector<float>>
saxpy(float a, std::span<const float> x, std::span<const float> y) {
    if (x.size() != y.size()) return std::nullopt;
    std::vector<float> out(x.size());
    if (opkit_saxpy(a, x.data(), y.data(), out.data(),
                    static_cast<unsigned>(x.size())) != 0)
        return std::nullopt;
    return out;
}

// WHICH backend answered. An operator library that computes and does not say
// where cannot be checked: every backend returns the same numbers, so the
// numbers alone do not separate a device run from the reference one.
std::string_view backend() { return opkit_backend(); }

} // namespace opkit
