// mcpp.toolchain.cenv_probe — the `[c-abi]` declaration is CHECKED, not TRUSTED.
//
// design 2026-09-18 §3.2: "声明被校验，而不是被信任" (a declaration is
// verified, never trusted) — the same rule openkal itself follows for its own
// conformance claims. Once the target side has resolved the tokens
// `mcpp.toolchain.cenv::realise` computed, this module asks the REAL
// compiler, with those REAL final flags, what it actually predefines, and
// compares the answer against what the C library declared. A mismatch fails
// the build and prints both — the declaration and the measurement — because
// a caller that is told only "wrong" has no way to tell which of the two was.
//
// ONE PREPROCESS, NO CODEGEN, NO EXECUTION. `-E -dM` dumps every macro the
// driver would hand the real compile — including `__SIZEOF_LONG__` and
// `__SIZEOF_WCHAR_T__`, which clang predefines from the SAME target-and-flags
// resolution that decides the real `sizeof(long)`/`sizeof(wchar_t)` — so the
// three facts §3.2 names (`sizeof(long)`, `__SIZEOF_WCHAR_T__`,
// `_WIN32`/`__unix__` presence) are all read from one dump. No execution: the
// realised environment is routinely a CROSS target (`x86_64-windows-gnu`
// built on Linux is the design's own motivating case), and a probe that had
// to RUN the result would need the target to be runnable on the build host,
// which defeats a check meant to hold for every build.
//
// CACHED PER CONFIGURATION. The dump depends only on the compiler binary and
// the exact argv handed to it — nothing about the project's own sources — so
// it is hashed and cached beside the std module cache
// (`mcpp::home::cache_root()`), and a build that resolves the same
// configuration twice (two targets in one workspace, or two consecutive
// builds) pays for the compile once.
export module mcpp.toolchain.cenv_probe;

import std;
import mcpp.home;
import mcpp.platform;
import mcpp.toolchain.fingerprint;   // hash_string — the same 64-bit FNV the std cache keys with

export namespace mcpp::toolchain::cenv_probe {

struct Mismatch {
    std::string fact;       // "sizeof(long)" / "wchar_t width" / "_WIN32" / "__unix__" / ...
    std::string declared;
    std::string measured;
};

struct Result {
    bool                  ran = false;   // false = cache hit, nothing executed this call
    std::vector<Mismatch> mismatches;    // empty = the declaration held
};

// `compilerBin` + `argv` is the EXACT final compile configuration an
// ordinary target-side unit receives — the base host-compile tokens plus
// `Toolchain::cEnvTokens`/`cEnvBuiltinsTokens` (`mcpp.toolchain.cenv`).
// `expectWcharBits`/`expectLongBytes` 0 = not checked; the `expectDefined`/
// `expectUndefined` lists name macros the probe's `-dM` dump must and must
// not contain.
//
// `hostStripMacros` — PRECOMPILE `-U<name>` TOKENS THE CALLER INSERTS INTO THE
// PROBE COMMAND BEFORE `-E -dM`. This is the only mechanism by which the
// probe is allowed to compensate for a HOST contamination that the target's
// own `--target=` does not neutralise; the four Windows-host names
// (`_WIN32`, `_WIN64`, `__MINGW32__`, `__MINGW64__`) are the case this was
// added for and the only set the build's own measurements have surfaced, but
// the parameter is a list because the design does not commit to this set
// being the final one — a future host whose compiler leaks a different macro
// can be handled the same way without further changes to this module.
//
// Why this is in the probe and not in `mcpp.toolchain.cenv::realise`: the
// probe's job is to MEASURE what the compiler ACTUALLY does for the target,
// not to ask the compiler what it would do for the target if it were a
// clean cross compile. Host contamination is a defect of the measurement, not
// of the measurement's contract — design 2026-09-18 §3.2 names a declaration
// as "checked, not trusted", and the check has to read the macro state the
// compile would actually deliver to a real translation unit, not the state
// the compile would deliver to one already stripped of every fact the host
// carried. The strip happens before `-E -dM` so the dump reflects the
// stripped state; without it, a `present = "posix"` declaration would always
// "fail" on a Windows host because `_WIN32` is in the dump regardless of
// `--target=`.
//
// The cache key (below) folds `hostStripMacros` in alongside the rest of the
// argv, so two callers with the same compiler and argv but different strip
// sets do not share a slot.
//
// A refusal here (as opposed to a non-empty `mismatches`) means the probe
// itself could not run — the compiler rejected the command line, which is a
// DIFFERENT failure from the declaration disagreeing with what compiled: the
// caller reports it as a build error naming the command, not as a §3.2
// verification mismatch.
//
// `hostStripMacros` is placed AFTER `cacheRoot` (the latter being the test
// suite's frequent override) to keep the existing call sites — which pass
// neither — source-compatible. New callers that DO need the strip supply
// both arguments; tests that pin a temp cache directory pass the strip
// empty by default.
std::expected<Result, std::string> verify(
    const std::filesystem::path& compilerBin,
    const std::vector<std::string>& argv,
    int expectWcharBits, int expectLongBytes,
    const std::vector<std::string>& expectDefined,
    const std::vector<std::string>& expectUndefined,
    const std::filesystem::path& cacheRoot = mcpp::home::cache_root(),
    const std::vector<std::string>& hostStripMacros = {});

} // namespace mcpp::toolchain::cenv_probe

namespace mcpp::toolchain::cenv_probe {

namespace {

// One line per predefined macro's NAME, ignoring its expansion — every fact
// this module checks is "is this name defined at all", which is what
// `_WIN32`/`__unix__`/`__CYGWIN__` answer and what `expectDefined`/
// `expectUndefined` ask about. `__SIZEOF_LONG__`/`__SIZEOF_WCHAR_T__` are the
// two exceptions and are looked up with their value kept.
struct MacroDump {
    std::set<std::string>            names;
    std::map<std::string, std::string> values;
};

MacroDump parse_dm(std::string_view out) {
    MacroDump d;
    std::size_t pos = 0;
    while (pos < out.size()) {
        auto nl = out.find('\n', pos);
        std::string_view line = out.substr(pos, nl == std::string_view::npos
            ? std::string_view::npos : nl - pos);
        pos = (nl == std::string_view::npos) ? out.size() : nl + 1;
        // `#define NAME value...` — clang's -dM format, one per predefined
        // macro. `NAME` may itself take parameters (`#define FOO(x) ...`);
        // this module only ever looks up object-like macro names, so the
        // parenthesised form is simply never matched by `names.contains`.
        if (!line.starts_with("#define ")) continue;
        auto rest = line.substr(8);
        auto sp = rest.find(' ');
        auto name = std::string(sp == std::string_view::npos ? rest : rest.substr(0, sp));
        d.names.insert(name);
        if (sp != std::string_view::npos)
            d.values[name] = std::string(rest.substr(sp + 1));
    }
    return d;
}

} // namespace

std::expected<Result, std::string> verify(
    const std::filesystem::path& compilerBin,
    const std::vector<std::string>& argv,
    int expectWcharBits, int expectLongBytes,
    const std::vector<std::string>& expectDefined,
    const std::vector<std::string>& expectUndefined,
    const std::filesystem::path& cacheRoot,
    const std::vector<std::string>& hostStripMacros) {

    // The cache key is the compiler binary's own identity plus every argv
    // token, in order — exactly the inputs that can change what `-dM`
    // prints. mcpp's own content hash (`hash_file`) would need to re-read
    // the binary on every build; the path plus its last-write time is the
    // same shortcut the toolchain probe elsewhere in this codebase already
    // takes for "has this compiler changed". `hostStripMacros` is folded in
    // for the same reason `argv` is: two callers that probe the same
    // compiler + argv with different strip lists must not share a cache
    // slot, because the dumps WILL differ.
    std::error_code ec;
    auto mtime = std::filesystem::last_write_time(compilerBin, ec);
    std::string keyInput = compilerBin.string();
    keyInput += '\x1f';
    keyInput += std::to_string(
        static_cast<long long>(mtime.time_since_epoch().count()));
    for (auto& a : argv) { keyInput += '\x1f'; keyInput += a; }
    for (auto& s : hostStripMacros) { keyInput += '\x1f'; keyInput += s; }
    const std::string key = mcpp::toolchain::hash_string(keyInput);

    const auto cacheDir = cacheRoot / "cenv-probe";
    const auto cacheFile = cacheDir / (key + ".dm");

    std::string dump;
    bool ran = false;
    if (std::ifstream cached(cacheFile, std::ios::binary); cached) {
        std::ostringstream ss; ss << cached.rdbuf();
        dump = ss.str();
    } else {
        // `-E -dM`: preprocess only, dump every predefined macro. No parse,
        // no codegen — the cheapest command that still asks the real driver,
        // with the real final flags, what it predefines. `-x c++`: every
        // caller of this module probes a C++-capable configuration (the
        // realised environment applies to C and C++ alike; C++ is the
        // superset for the macros this checks). `-`: read the (empty) source
        // from standard input — `capture_stdout` gives the child an empty
        // one, argv-form, so no shell and no temp file are needed.
        //
        // `hostStripMacros` go BEFORE `argv` so they strip host predefines
        // before `--target=` (or any other token in argv) takes effect; an
        // `-U` placed after `--target=` still strips the macro (clang
        // processes `-U` in order), but keeping them in front makes the
        // intent obvious in the recorded command and keeps the strip
        // orthogonal to whatever the target-side configuration produces.
        std::vector<std::string> cmd{ compilerBin.string() };
        cmd.insert(cmd.end(), hostStripMacros.begin(), hostStripMacros.end());
        cmd.insert(cmd.end(), argv.begin(), argv.end());
        cmd.insert(cmd.end(), { "-x", "c++", "-E", "-dM", "-" });
        auto r = mcpp::platform::process::capture_stdout(cmd);
        if (r.exit_code != 0)
            return std::unexpected(std::format(
                "the [c-abi] verification probe could not be compiled "
                "(rc={}). command: {}", r.exit_code,
                std::accumulate(cmd.begin(), cmd.end(), std::string(),
                    [](std::string a, std::string const& b) {
                        return a.empty() ? b : a + " " + b;
                    })));
        dump = r.output;
        ran = true;
        std::error_code mkec;
        std::filesystem::create_directories(cacheDir, mkec);
        std::ofstream out(cacheFile, std::ios::binary);
        if (out) out << dump;
    }

    auto macros = parse_dm(dump);
    Result res;
    res.ran = ran;

    auto sizeof_from = [&](std::string_view macro) -> std::optional<int> {
        auto it = macros.values.find(std::string(macro));
        if (it == macros.values.end()) return std::nullopt;
        // The value is a decimal integer literal, occasionally with a
        // trailing suffix (`4U`, on some targets) — read the leading digits
        // only.
        int v = 0;
        auto s = it->second;
        std::size_t i = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); ++i; }
        if (i == 0) return std::nullopt;
        return v;
    };

    if (expectLongBytes != 0) {
        if (auto got = sizeof_from("__SIZEOF_LONG__"); got && *got != expectLongBytes)
            res.mismatches.push_back({"sizeof(long)",
                std::to_string(expectLongBytes), std::to_string(*got)});
    }
    if (expectWcharBits != 0) {
        if (auto got = sizeof_from("__SIZEOF_WCHAR_T__"); got
            && *got * 8 != expectWcharBits)
            res.mismatches.push_back({"__SIZEOF_WCHAR_T__ (bits)",
                std::to_string(expectWcharBits), std::to_string(*got * 8)});
    }
    for (auto& name : expectDefined)
        if (!macros.names.contains(name))
            res.mismatches.push_back({name, "defined", "undefined"});
    for (auto& name : expectUndefined)
        if (macros.names.contains(name))
            res.mismatches.push_back({name, "undefined", "defined"});

    return res;
}

} // namespace mcpp::toolchain::cenv_probe
