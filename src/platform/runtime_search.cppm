// mcpp.platform.runtime_search — where a produced artifact looks for its
// shared libraries at RUN time, in what order, and which of those directories
// may not travel with it.
//
// THE ASYMMETRY THIS EXISTS TO CLOSE
//
// mcpp already treats the selected SubOS as its sysroot: `--sysroot=<subos>`
// goes on both the compile and the link line, so `-lGL` resolves out of
// `<subos>/lib` with no flags from the user. Nothing carried that same view
// into the RUN-time search path, which was derived separately from the
// toolchain's payload directories alone. A library that lives only in the
// SubOS therefore linked cleanly and produced an executable that cannot
// start:
//
//   $ mcpp build     → rc=0
//   $ ./bin/app      → libGL.so.1: cannot open shared object file
//
// One decision — "the SubOS is my root" — was being derived twice, and the
// second derivation had a smaller input set.
//
// WHY A MODULE RATHER THAN A VECTOR PASSED AROUND
//
// Four parties must agree on the ORDER and on what "machine-local" means:
// the linker command line (`mcpp.build.plan`), the closure resolver
// (`mcpp.platform.elf_runtime`), `mcpp pack` (which must strip what cannot be
// shipped), and the recorder (`resolution.json`). A per-caller decision is how
// this area produced a "validation: pass" for a binary that could not load.
//
// This module knows nothing about ELF, about SubOS layout, or about
// RuntimeBinding. It is the policy — rank, provenance, machine-locality — and
// it imports only `std`, so it can be unit-tested on every platform including
// the ones where none of it applies.

export module mcpp.platform.runtime_search;

import std;

export namespace mcpp::platform::search {

// Where a runtime search directory came from.
//
// The distinction that matters is MUTABILITY, not ownership: two of these are
// immutable once installed, one is rewritten by every `xlings install`, and
// one belongs to a different machine's loader entirely.
enum class Origin {
    // An immutable package payload — `<store>/xim-x-glibc/2.39/lib64`.
    // Written once at install time and never touched again.
    Payload,
    // Declared by a resolved package descriptor's `[runtime]` block. Also
    // store-backed and immutable, but it is the ecosystem's statement rather
    // than the toolchain's, so it is ranked separately and can be reported
    // separately.
    Package,
    // The SubOS symlink farm — `<subos>/lib`. A UNION VIEW of everything
    // installed into that environment, rewritten whenever the environment is
    // re-resolved. This is the directory that makes `-lGL` link, and the one
    // that must never outrank a payload.
    SubosFarm,
    // The host loader's own built-in defaults (`/usr/lib/x86_64-linux-gnu`,
    // `/lib64`, …). Applies ONLY when the artifact will actually run under
    // the host loader. An artifact whose PT_INTERP points into a private
    // payload never consults these, and pretending otherwise is exactly how a
    // binary that cannot start was reported as valid: the host happened to
    // have `libGL.so.1`, so the model resolved it and the loader could not.
    HostDefault,
};

// Search order = decreasing immutability.
//
// This is the one invariant in this module, and it is not stylistic:
//
//   payload first  ⇒ libc / libm / libstdc++ always resolve from the pinned
//                    payload, and the farm only supplies what nothing else
//                    does (libGL, libX11, libEGL, libwayland, …).
//   farm first     ⇒ a later `xlings install` re-points the farm and silently
//                    changes which libc an ALREADY BUILT artifact loads.
//
// Today the farm's `libc.so.6` is a symlink to that same payload, so the two
// orders behave identically — which is precisely why the rule has to be
// written down and asserted rather than left to chance.
int rank(Origin origin) {
    switch (origin) {
        case Origin::Payload:     return 0;
        case Origin::Package:     return 1;
        case Origin::SubosFarm:   return 2;
        case Origin::HostDefault: return 3;
    }
    return 3;
}

// Is this directory part of THIS machine's private state?
//
// `mcpp pack` asks this to decide what may not be baked into a distributable
// artifact. A path under the build machine's store or SubOS is meaningless —
// or worse, silently different — anywhere else.
//
// `HostDefault` is NOT machine-local: `/usr/lib` is a convention every Linux
// target machine shares, and a binary relying on it is making an ordinary
// (declarable) host requirement rather than depending on the build box.
bool is_machine_local(Origin origin) {
    switch (origin) {
        case Origin::Payload:     return true;
        case Origin::SubosFarm:   return true;
        case Origin::Package:     return true;
        case Origin::HostDefault: return false;
    }
    return false;
}

std::string_view to_string(Origin origin) {
    switch (origin) {
        case Origin::Payload:     return "payload";
        case Origin::Package:     return "package";
        case Origin::SubosFarm:   return "subos_farm";
        case Origin::HostDefault: return "host_default";
    }
    return "unknown";
}

std::optional<Origin> parse_origin(std::string_view value) {
    if (value == "payload")      return Origin::Payload;
    if (value == "package")      return Origin::Package;
    if (value == "subos_farm")   return Origin::SubosFarm;
    if (value == "host_default") return Origin::HostDefault;
    return std::nullopt;
}

struct Dir {
    std::filesystem::path path;
    Origin origin = Origin::Payload;

    bool operator==(const Dir&) const = default;
};

// THE single ordering. Stable within a rank, so insertion order still decides
// among peers (which is what libglvnd's vendor resolution depends on), and
// de-duplicated by path with the STRONGEST origin winning.
//
// Strongest wins rather than first-seen: the same directory reachable both as
// a payload and through the farm view is a payload — the farm is only an
// alternate route to it, and ranking it as farm would push the real libc
// behind whatever else the farm holds.
std::vector<Dir> ordered(std::vector<Dir> dirs) {
    // `lexically_normal` KEEPS a trailing separator (`/a/./b/` → `/a/b/`), and
    // a path with one does not compare equal to the same path without. These
    // are all directories, so the separator carries no information — leaving
    // it in means the same directory can enter the closure twice, under two
    // different origins, and the de-duplication that keeps the farm behind the
    // payload silently stops matching. Caught by a unit test, not by reading.
    auto normalize = [](const std::filesystem::path& p) {
        auto n = p.lexically_normal();
        if (!n.has_filename() && n.has_parent_path()) return n.parent_path();
        return n;
    };
    std::vector<Dir> merged;
    for (auto& dir : dirs) {
        if (dir.path.empty()) continue;
        auto normalized = normalize(dir.path);
        auto hit = std::ranges::find_if(merged, [&](Dir const& existing) {
            return existing.path == normalized;
        });
        if (hit == merged.end()) {
            merged.push_back({std::move(normalized), dir.origin});
            continue;
        }
        if (rank(dir.origin) < rank(hit->origin)) hit->origin = dir.origin;
    }
    std::ranges::stable_sort(merged, {}, [](Dir const& d) { return rank(d.origin); });
    return merged;
}

// Just the paths, in contract order — for the callers that render a flag or
// walk a search path and have no use for the provenance.
std::vector<std::filesystem::path> paths_of(std::span<const Dir> dirs) {
    std::vector<std::filesystem::path> out;
    out.reserve(dirs.size());
    for (auto const& dir : dirs) out.push_back(dir.path);
    return out;
}

// The declarative exit from xlings' linker wrapper (openxlings/xlings#540).
//
// That wrapper appends `-rpath "$XLINGS_SUBOS_LIB" --disable-new-dtags` to
// every link it sees. mcpp wants the tag half and must refuse the path half:
// `$XLINGS_SUBOS_LIB` names the ACTIVE shell's SubOS, which is measurably not
// the one mcpp resolved — mcpp keeps its own xlings home under
// `<mcpp home>/registry`, so the variable points at a different farm backed by
// a different physical glibc payload. Inheriting it would put a second libc on
// the artifact's search path, which is the one thing rule B exists to prevent.
//
// mcpp declares the exit BEFORE the wrapper ships. Today it is a no-op; the
// day the wrapper lands it keeps mcpp's DT_RPATH exactly what mcpp decided.
// Set in mcpp's own process environment rather than per link command: the link
// line has a hard 128KiB ceiling that real workspaces already spend 43% of,
// and children inherit the environment anyway.
inline constexpr std::string_view kLinkerPathInjectionOptOut =
    "XLINGS_SUBOS_LD_PATHS";
inline constexpr std::string_view kLinkerPathInjectionOptOutValue = "0";

} // namespace mcpp::platform::search
