// mcpp.toolchain.abi — dimensional ABI compatibility model.
//
// Background: "ABI compatibility" between a dependency and the resolved
// toolchain is NOT a single flat axis. It has independent dimensions:
//
//   libc       glibc | musl | msvcrt | macos   ← from the TARGET TRIPLE
//   cxxStdlib  libstdc++ | libc++ | msvc-stl    ← from the C++ STANDARD LIBRARY
//   arch       x86_64 | aarch64 | …             ← from the target triple
//   os         linux | darwin | windows         ← from the target triple
//   cxxAbi     itanium | msvc                    ← from the compiler
//
// A *C library* (e.g. glfw) only constrains `libc` — linking a glibc C
// library into a libc++ C++ program is fine, because libc++ is a C++ stdlib,
// not a libc replacement (it still links glibc on `*-linux-gnu`). The old
// single-string derivation conflated `libc` with `cxxStdlib` (a libc++
// toolchain was labelled abi=libc++ and wrongly rejected `abi:glibc` deps).
//
// This module models each dimension independently. A dependency declares a
// constraint only on the dimension(s) it cares about; UNSPECIFIED DIMENSIONS
// ARE DON'T-CARE. See
// .agents/docs/2026-06-27-abi-compat-model-single-pr-design.md and
// .agents/docs/2026-06-27-glfw-abi-glibc-vs-libcxx-conflation-analysis.md.
export module mcpp.toolchain.abi;

import std;
import mcpp.toolchain.model;
import mcpp.toolchain.triple;

export namespace mcpp::toolchain {

enum class AbiDim { Libc, CxxStdlib, Arch, Os, CxxAbi };

inline std::string_view dim_name(AbiDim d) {
    switch (d) {
        case AbiDim::Libc:      return "libc";
        case AbiDim::CxxStdlib: return "cxxstdlib";
        case AbiDim::Arch:      return "arch";
        case AbiDim::Os:        return "os";
        case AbiDim::CxxAbi:    return "cxxabi";
    }
    return "?";
}

// A fully-resolved toolchain's ABI coordinates. Empty string on a dimension
// means "unknown / not detected" → that dimension never causes a mismatch.
struct AbiProfile {
    std::string libc;
    std::string cxxStdlib;
    std::string arch;
    std::string os;
    std::string cxxAbi;

    std::string_view get(AbiDim d) const {
        switch (d) {
            case AbiDim::Libc:      return libc;
            case AbiDim::CxxStdlib: return cxxStdlib;
            case AbiDim::Arch:      return arch;
            case AbiDim::Os:        return os;
            case AbiDim::CxxAbi:    return cxxAbi;
        }
        return {};
    }
};

// The single derivation site. libc/arch/os come from the target triple
// (parsed by triple.cppm — the one triple parser); cxxStdlib/cxxAbi come
// from the compiler. NO cross-dimension inference (that conflation was the
// bug). os vocabulary is the canonical one: linux | macos | windows —
// this used to say "darwin" while cfg() said "macos" for the same concept;
// converged on "macos" (parse_abi_capability accepts "darwin" as an alias).
inline AbiProfile abi_profile(const Toolchain& tc) {
    AbiProfile p;

    if (auto t = mcpp::toolchain::triple::parse(tc.targetTriple)) {
        p.arch = t->arch;
        p.os   = t->os;
        // libc — the C runtime ABI a C library links against. Windows is
        // msvcrt for BOTH env=msvc and env=gnu (MinGW links the Windows CRT).
        if      (t->is_musl())        p.libc = "musl";
        else if (t->os == "macos")    p.libc = "macos";
        else if (t->os == "windows")  p.libc = "msvcrt";
        else if (t->os == "linux")    p.libc = "glibc";   // *-linux-gnu (gcc AND clang+libc++)
        p.cxxStdlib = tc.stdlibId;
        p.cxxAbi = (tc.compiler == CompilerId::MSVC || t->is_msvc_env())
                       ? "msvc" : "itanium";
        return p;
    }

    // Fallback for triples outside the language (leave dims empty =
    // don't-care where nothing is recognizable).
    const std::string& t = tc.targetTriple;
    auto has = [&](std::string_view s) { return t.find(s) != std::string::npos; };
    if      (has("musl"))                  p.libc = "musl";
    else if (has("darwin") || has("apple")) p.libc = "macos";
    else if (has("msvc"))                  p.libc = "msvcrt";
    else if (has("windows"))               p.libc = "msvcrt";
    else if (has("linux") || has("gnu"))   p.libc = "glibc";
    if      (has("linux"))                  p.os = "linux";
    else if (has("darwin") || has("apple")) p.os = "macos";
    else if (has("windows"))               p.os = "windows";
    if (auto dash = t.find('-'); dash != std::string::npos) p.arch = t.substr(0, dash);
    else                                                    p.arch = t;
    p.cxxStdlib = tc.stdlibId;
    p.cxxAbi = (tc.compiler == CompilerId::MSVC || has("msvc")) ? "msvc" : "itanium";
    return p;
}

// One dimensional constraint a dependency places on the toolchain.
struct AbiConstraint {
    AbiDim      dim;
    std::string value;
    std::string source;   // provenance (provider package) for diagnostics
};

// Parse an `abi:` runtime capability string into a dimensional constraint:
//   legacy bare form  "abi:glibc" / "abi:musl" / "abi:msvcrt"  → libc dimension
//   dimensional form  "abi:<dim>=<value>"  e.g. "abi:cxxstdlib=libc++"
// Returns nullopt for non-`abi:` capabilities or an unknown dimension name
// (unknown dims are ignored for forward-compatibility).
inline std::optional<AbiConstraint>
parse_abi_capability(std::string_view cap, std::string_view source = {}) {
    constexpr std::string_view kPrefix = "abi:";
    if (cap.size() < kPrefix.size() || cap.substr(0, kPrefix.size()) != kPrefix)
        return std::nullopt;
    std::string_view body = cap.substr(kPrefix.size());

    auto eq = body.find('=');
    if (eq == std::string_view::npos)
        return AbiConstraint{ AbiDim::Libc, std::string(body), std::string(source) };

    std::string_view dimStr = body.substr(0, eq);
    std::string_view val    = body.substr(eq + 1);
    AbiDim dim;
    if      (dimStr == "libc")      dim = AbiDim::Libc;
    else if (dimStr == "cxxstdlib") dim = AbiDim::CxxStdlib;
    else if (dimStr == "arch")      dim = AbiDim::Arch;
    else if (dimStr == "os")        dim = AbiDim::Os;
    else if (dimStr == "cxxabi")    dim = AbiDim::CxxAbi;
    else return std::nullopt;
    // Vocabulary aliases → canonical (profiles emit macos / aarch64).
    std::string v(val);
    if (dim == AbiDim::Os   && v == "darwin") v = "macos";
    if (dim == AbiDim::Arch && v == "arm64")  v = "aarch64";
    return AbiConstraint{ dim, std::move(v), std::string(source) };
}

struct AbiMismatch {
    AbiDim      dim;
    std::string need;
    std::string got;
    std::string source;
};

// Check a toolchain profile against a set of dimensional constraints.
// A dimension the profile doesn't pin (empty) can't conflict. Returns all
// mismatches (empty vector = compatible).
inline std::vector<AbiMismatch>
abi_check(const AbiProfile& prof, const std::vector<AbiConstraint>& constraints) {
    std::vector<AbiMismatch> out;
    for (auto& c : constraints) {
        std::string_view got = prof.get(c.dim);
        if (got.empty()) continue;
        if (got != c.value)
            out.push_back({ c.dim, c.value, std::string(got), c.source });
    }
    return out;
}

} // namespace mcpp::toolchain
