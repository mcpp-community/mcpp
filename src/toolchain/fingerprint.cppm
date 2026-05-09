// mcpp.toolchain.fingerprint — 10-field fingerprint that gates BMI cache safety.
//
// Per docs/06-toolchain-and-fingerprint.md, the fingerprint MUST cover:
//   1.  compiler id           2.  compiler version
//   3.  compiler binary hash  4.  target triple
//   5.  stdlib id+version     6.  C++ standard
//   7.  compile flags hash    8.  mcpp version
//   9.  dependency lock hash  10. std module BMI hash
//
// MVP uses FNV-1a 64-bit (deterministic, collision-rare-enough for cache
// invalidation); spec calls for SHA-256 but for "did anything change?"
// FNV-1a is operationally sufficient. Swappable later.

export module mcpp.toolchain.fingerprint;

import std;
import mcpp.toolchain.detect;

export namespace mcpp::toolchain {

inline constexpr std::string_view MCPP_VERSION = "0.0.4";

struct FingerprintInputs {
    Toolchain                       toolchain;
    std::string                     cppStandard      = "c++23";
    std::string                     compileFlags;       // canonicalized
    std::string                     dependencyLockHash;// hex string of mcpp.lock
    std::string                     stdBmiHash;        // hex string of pre-built std.gcm
};

struct Fingerprint {
    std::string                     hex;                 // 16 hex chars
    std::array<std::string, 10>     parts;               // each field's stringified form
};

std::string hash_file(const std::filesystem::path& p);     // returns 16 hex
std::string hash_string(std::string_view s);               // returns 16 hex
Fingerprint compute_fingerprint(const FingerprintInputs& in);

} // namespace mcpp::toolchain

namespace mcpp::toolchain {

namespace {

inline std::string to_hex16(std::uint64_t h) {
    // Hand-rolled to avoid `std::format("{:016x}", ...)` which throws on
    // GCC 15's `import std` (libstdc++ partial format support).
    static constexpr char hex[] = "0123456789abcdef";
    char buf[16];
    for (int i = 15; i >= 0; --i) { buf[i] = hex[h & 0xf]; h >>= 4; }
    return std::string(buf, 16);
}

inline std::uint64_t fnv1a_64(std::string_view data, std::uint64_t seed = 0xcbf29ce484222325ull) {
    std::uint64_t h = seed;
    for (unsigned char c : data) {
        h ^= c;
        h *= 0x100000001b3ull;
    }
    return h;
}

inline std::uint64_t fnv1a_64_file(const std::filesystem::path& p) {
    std::ifstream is(p, std::ios::binary);
    if (!is) return 0;
    std::array<char, 8192> buf;
    std::uint64_t h = 0xcbf29ce484222325ull;
    while (is) {
        is.read(buf.data(), buf.size());
        std::streamsize got = is.gcount();
        for (std::streamsize i = 0; i < got; ++i) {
            h ^= static_cast<unsigned char>(buf[i]);
            h *= 0x100000001b3ull;
        }
    }
    return h;
}

} // namespace

std::string hash_file(const std::filesystem::path& p) {
    return to_hex16(fnv1a_64_file(p));
}

std::string hash_string(std::string_view s) {
    return to_hex16(fnv1a_64(s));
}

Fingerprint compute_fingerprint(const FingerprintInputs& in) {
    Fingerprint fp;
    auto& tc = in.toolchain;

    fp.parts[0] = std::string(tc.compiler_name());
    fp.parts[1] = tc.version;
    fp.parts[2] = tc.binaryPath.empty() ? "" : hash_file(tc.binaryPath);
    fp.parts[3] = tc.targetTriple;
    fp.parts[4] = std::format("{} {}", tc.stdlibId, tc.stdlibVersion);
    fp.parts[5] = in.cppStandard;
    fp.parts[6] = hash_string(in.compileFlags);
    fp.parts[7] = std::string(MCPP_VERSION);
    fp.parts[8] = in.dependencyLockHash;
    fp.parts[9] = in.stdBmiHash;

    // Combine all parts deterministically.
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (auto& p : fp.parts) {
        h = fnv1a_64(p, h);
        h = fnv1a_64("\x1f", h);   // unit separator
    }
    fp.hex = to_hex16(h);
    return fp;
}

} // namespace mcpp::toolchain
