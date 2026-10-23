// mcpp.toolchain.post_install — what an installed toolchain payload says
// about the runtime it was built against, read and never written: its baked
// runtime binding, the pristine link spec a build generates into its own
// directory, the glibc payload a runtime identity names.
//
// A payload is immutable (design 2026-10-16 §7): the post-install rewrites
// that used to live here (PT_INTERP/RUNPATH by patchelf, gcc specs, clang cfg,
// the `.mcpp-fixup.json` marker) are gone. A toolchain runs on the runtime
// its provider bound it to; what mcpp's artifacts load is decided per build
// (mcpp.toolchain.linkmodel).

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.toolchain.post_install;

import std;
import mcpp.config;
import mcpp.libs.json;
import mcpp.log;
import mcpp.platform;
import mcpp.runtime.binding;
import mcpp.toolchain.linkmodel;
import mcpp.toolchain.registry;
import mcpp.ui;
import mcpp.provider;
import mcpp.provider.select;

namespace mcpp::toolchain {

// Extract the baked-in glibc loader path (".../ld-linux-<arch>.so.N") from a
// gcc specs file. xim bakes the installing user's XLINGS_HOME into specs at
// install time, so the DIR varies per machine, and the loader NAME varies
// per arch — detect both instead of hardcoding either.
// The runtime binding a toolchain was INSTALLED against, read back out of
// what mcpp itself wrote: gcc's specs, clang's cfg.
//
// Compatibility only. The authority is the subos's `subos_info.runtime`; this
// answers for machines whose subos predates that block, and it answers with
// the version the artifact will actually load -- so compile and run still
// agree, which is the invariant that matters.
//
// Returns "glibc@<ver>" or empty. The version comes from the payload path
// (.../xim-x-glibc/<ver>/lib64), which is the only place it is written down.
export std::string baked_runtime_binding(const std::filesystem::path& compilerBin);

// Write a specs file that REPLACES gcc's `*link:` with the pristine built-in
// one, and return its path (empty when not applicable or on failure).
//
// Why this exists: `-rpath` accumulates. mcpp emitting its own does not remove
// what the payload's specs already inject, and that file has been patched by
// every home that ever installed against this shared payload -- one stale
// entry per run, forever. Replacing the spec is the only way to stop them
// reaching a user's artifact, and it works on machines already polluted.
//
// The pristine `*link:` comes from `g++ -dumpspecs`, which prints the specs
// COMPILED INTO the binary and is unaffected by the file on disk. So the
// original is always recoverable and the payload is never touched -- an
// earlier plan to reinstall the toolchain was unnecessary.
//
// `-specs=<file>` whose definition does not begin with `+` replaces rather
// than appends; both properties were measured before this was written.
export std::filesystem::path write_clean_link_specs(
    const std::filesystem::path& compilerBin,
    const std::filesystem::path& outputDir);

export std::string detect_baked_loader(const std::string& specsContent) {
    // Path-character whitelist. Specs embed loader paths inside %-spec
    // syntax (`%{mmusl:...;:/baked/dir/ld-linux-x86-64.so.2}`), so scanning
    // to "whitespace or :;" is NOT a valid boundary — it would swallow the
    // closing braces, and replacing that string corrupts the spec grammar
    // ("braced spec body ... is invalid" from every subsequent g++ run).
    auto is_path_char = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c))
            || c == '/' || c == '.' || c == '-' || c == '_' || c == '+';
    };

    // The baked GNU loader is the ld-linux entry whose directory is NOT a
    // standard /lib* location — specs also contain pristine defaults
    // (/lib/ld-linux.so.2, /libx32/…) for other multilib branches that must
    // never be rewritten.
    constexpr std::string_view kLoaderMark = "/ld-linux-";
    for (std::size_t pos = specsContent.find(kLoaderMark);
         pos != std::string::npos;
         pos = specsContent.find(kLoaderMark, pos + 1)) {
        auto start = pos;
        while (start > 0 && is_path_char(specsContent[start - 1])) --start;
        auto end = pos + 1;
        while (end < specsContent.size() && is_path_char(specsContent[end])) ++end;
        auto loader = specsContent.substr(start, end - start);
        if (loader.empty() || loader[0] != '/') continue;
        auto dir = std::filesystem::path(loader).parent_path().string();
        if (dir == "/lib" || dir == "/lib64" || dir == "/lib32" || dir == "/libx32")
            continue;  // pristine multilib default, not a baked path
        return loader;
    }
    return "";
}

// fixup_gcc_specs is GONE.
//
// It wrote the loader and the rpath into the gcc payload's `specs` at install
// time. Both are now emitted per build by mcpp.toolchain.linkmodel, which
// removes an answerer rather than adding a path:
//
//   - the RUN side was a per-toolchain-install decision while the COMPILE
//     side was per-build, and they named different glibc versions the moment
//     a second one was installed;
//   - the substitution had a one-path needle and a two-path replacement, so
//     every home that patched the SHARED payload left one entry behind. 68 on
//     a developer machine, all pointing at deleted mktemp directories, in
//     every gcc artifact it produced.
//
// The payload is no longer written to at all, which restores what R6 asks of
// it: immutable, and independent of any particular home. A build that needs
// a clean `*link:` gets one generated into its own build directory (see
// write_clean_link_specs) -- the pristine spec comes from `g++ -dumpspecs`,
// which prints what is compiled into the binary and is therefore always
// recoverable.
//

// Resolve one glibc payload from the RuntimeBinding identity. The lookup is
// exact: `glibc@2.44.3` means the `2.44.3` directory and no other, which is
// the name xlings gives the payload of that version. Exported so the rule is
// pinned by a pure unit test without installing a toolchain.
//
// THERE IS NO FALLBACK TO A NEIGHBOURING VERSION. A scan that accepted any
// `2.44.x` for `2.44` existed to tolerate a payload installed as a side effect
// of some other install; it concealed that the declared payload itself was
// never installed, and it refused as soon as two revisions were present
// (mcpp#660). The declared payload is now installed by
// `ensure_declared_runtime` before any fixup, so an absent directory is a
// fact to report, with the coordinate that provides it.
export std::expected<std::filesystem::path, std::string>
select_glibc_payload_lib(const std::filesystem::path& glibcRoot,
                         std::string_view runtimeId) {
    constexpr std::string_view prefix = "glibc@";
    if (!runtimeId.starts_with(prefix)) {
        // Name the PROVIDER that was found, not just the one that was
        // wanted. `ucrt@10.0.26100.0` is a perfectly valid runtime identity
        // that this fixup has nothing to do with (there is no ucrt payload to
        // bind to — it is an OS component), and "not a glibc payload
        // identity" reads as "malformed" for it.
        auto provider = mcpp::platform::runtime::runtime_provider(runtimeId);
        return std::unexpected(provider.empty()
            ? std::format(
                  "selected RuntimeBinding '{}' is not a glibc payload identity "
                  "(expected glibc@<version>)", runtimeId)
            : std::format(
                  "selected RuntimeBinding '{}' names the '{}' runtime "
                  "provider; the glibc payload fixup does not apply to it",
                  runtimeId, provider));
    }
    auto version = runtimeId.substr(prefix.size());
    if (version.empty() || version == "." || version == ".."
        || version.find('/') != std::string_view::npos
        || version.find('\\') != std::string_view::npos
        || version.find('@') != std::string_view::npos) {
        return std::unexpected(std::format(
            "selected RuntimeBinding '{}' has an invalid glibc version",
            runtimeId));
    }

    const auto payload = glibcRoot / std::string(version);
    std::error_code ec;
    if (!std::filesystem::is_directory(payload, ec)) {
        return std::unexpected(std::format(
            "the declared runtime {} is not installed: '{}' does not exist. "
            "It is installed on first use when the network is available; for an "
            "offline build, provide `xim:{}` in the xlings store beforehand",
            runtimeId, payload.string(), runtimeId));
    }
    auto lib = payload_lib_dir_with_loader(payload);
    if (lib.empty()) {
        return std::unexpected(std::format(
            "selected RuntimeBinding {} payload '{}' is stale/incomplete: no "
            "dynamic loader was found under lib64/ or lib/",
            runtimeId, payload.string()));
    }
    return lib;
}

// WHICH TOOLCHAIN PAYLOADS RUN ON A C RUNTIME PAYLOAD, stated once: a GNU
// gcc and an llvm on Linux. The runtime they run on is the one the provider
// bound them to when it installed them (its home's runtime pin, design
// 2026-10-16 §7.2); mcpp makes sure that runtime is installed and writes
// nothing into the toolchain (§7.1: the patchelf, specs and cfg rewrites
// that once lived here are gone). Empty on every other payload and on
// Windows, where a PE toolchain has no C runtime payload.
export std::string_view runtime_payload_kind(const XimToolchainPackage& pkg) {
    if constexpr (mcpp::platform::is_windows) return {};
    if (pkg.needsGccPostInstallFixup) return "gcc";
    if (pkg.ximName == "llvm")        return "llvm";
    return {};
}

// PT_INTERP of an ELF, read directly.
//
// Not via `patchelf --print-interpreter`: this runs during prepare, where
// patchelf is not guaranteed to be resolved, and a dependency on an external
// tool for four fields of a header is a dependency that will be missing on
// exactly the machine that needs the answer.
std::string read_elf_interp(const std::filesystem::path& bin) {
    std::ifstream is(bin, std::ios::binary);
    if (!is) return {};
    unsigned char ident[16]{};
    is.read(reinterpret_cast<char*>(ident), sizeof ident);
    if (!is || ident[0] != 0x7f || ident[1] != 'E'
            || ident[2] != 'L' || ident[3] != 'F') return {};
    const bool is64 = ident[4] == 2;
    const bool le   = ident[5] == 1;
    if (!is64 || !le) return {};   // the only shape payloads ship

    auto u16 = [&](std::streamoff off) -> std::uint16_t {
        is.seekg(off); unsigned char b[2]{};
        is.read(reinterpret_cast<char*>(b), 2);
        return static_cast<std::uint16_t>(b[0] | (b[1] << 8));
    };
    auto u64 = [&](std::streamoff off) -> std::uint64_t {
        is.seekg(off); unsigned char b[8]{};
        is.read(reinterpret_cast<char*>(b), 8);
        std::uint64_t v = 0;
        for (int i = 7; i >= 0; --i) v = (v << 8) | b[i];
        return v;
    };

    const auto phoff     = u64(0x20);
    const auto phentsize = u16(0x36);
    const auto phnum     = u16(0x38);
    if (!is || phoff == 0 || phentsize < 0x38 || phnum == 0) return {};

    constexpr std::uint32_t kPtInterp = 3;
    for (std::uint16_t i = 0; i < phnum; ++i) {
        const auto ph = static_cast<std::streamoff>(phoff)
                      + static_cast<std::streamoff>(i) * phentsize;
        is.seekg(ph);
        unsigned char t[4]{};
        is.read(reinterpret_cast<char*>(t), 4);
        if (!is) return {};
        const std::uint32_t type =
            static_cast<std::uint32_t>(t[0]) | (t[1] << 8)
          | (t[2] << 16) | (static_cast<std::uint32_t>(t[3]) << 24);
        if (type != kPtInterp) continue;
        const auto offset = u64(ph + 0x08);
        const auto filesz = u64(ph + 0x20);
        if (filesz == 0 || filesz > 4096) return {};
        std::string str(static_cast<std::size_t>(filesz), '\0');
        is.seekg(static_cast<std::streamoff>(offset));
        is.read(str.data(), static_cast<std::streamsize>(filesz));
        if (!is) return {};
        if (auto z = str.find('\0'); z != std::string::npos) str.resize(z);
        return str;
    }
    return {};
}

std::string baked_runtime_binding(const std::filesystem::path& compilerBin) {
    if (compilerBin.empty()) return {};
    std::error_code ec;

    auto from_text = [](const std::string& text) -> std::string {
        auto loader = detect_baked_loader(text);
        if (loader.empty()) return {};
        // The recorded path may name a subos VIEW (`<home>/subos/default/lib/
        // ld-linux-...`), which is a symlink into the payload. R6: artifacts
        // bind the payload, never the mutable view -- so resolve to the
        // payload before reading a version off it. A view path has no version
        // component at all, and parsing one yields nothing.
        std::error_code cec;
        if (auto real = std::filesystem::canonical(loader, cec); !cec)
            loader = real.string();
        // .../xim-x-glibc/<ver>/lib64/ld-linux-... — the version is the
        // grandparent of the lib dir.
        auto dir = std::filesystem::path(loader).parent_path();   // lib64
        auto ver = dir.parent_path().filename().string();          // <ver>
        auto pkg = dir.parent_path().parent_path().filename().string();
        if (ver.empty() || pkg.find("glibc") == std::string::npos) return {};
        return "glibc@" + ver;
    };

    // Which glibc payloads are actually installed. Every source below is
    // checked against this, because all of them are RECORDS of a past state:
    // specs and cfg were written at some install, PT_INTERP at some fixup. A
    // payload can be replaced afterwards -- upgraded, garbage-collected -- and
    // the record keeps naming what used to be there.
    //
    // That is not hypothetical. CI resolved `glibc@2.39` from a record while
    // the only payload on disk was 2.44, so the exact-match probe correctly
    // refused, no payload paths resolved, and the artifact fell through to the
    // host loader. A fossil that names something no longer present is not an
    // authority; it is just old.
    std::vector<std::string> installed;
    if (auto xpkgs = mcpp::provider::store_of(compilerBin)) {
        for (auto it = std::filesystem::directory_iterator(mcpp::provider::layout().tool_in(*xpkgs, "glibc"), ec);
             !ec && it != std::filesystem::directory_iterator{}; it.increment(ec)) {
            if (!it->is_directory(ec)) continue;
            auto v = it->path().filename().string();
            if (!v.empty() && v.front() != '.') installed.push_back(v);
        }
    }
    auto still_there = [&](const std::string& binding) {
        if (binding.empty()) return false;
        const auto at = binding.find('@');
        if (at == std::string::npos) return false;
        auto ver = binding.substr(at + 1);
        return std::ranges::find(installed, ver) != installed.end();
    };

    auto read_file = [&](const std::filesystem::path& p) -> std::string {
        if (!std::filesystem::exists(p, ec)) return {};
        std::ifstream is(p);
        std::stringstream ss; ss << is.rdbuf();
        return ss.str();
    };

    // clang: the sibling <driver>.cfg.
    auto cfg = compilerBin.parent_path()
             / (compilerBin.stem().string() + ".cfg");
    if (auto r = from_text(read_file(cfg)); still_there(r)) return r;

    // gcc: lib/gcc/<triple>/<ver>/specs, one level of globbing each.
    auto gccRoot = compilerBin.parent_path().parent_path();
    for (auto t = std::filesystem::directory_iterator(gccRoot / "lib" / "gcc", ec);
         !ec && t != std::filesystem::directory_iterator{}; t.increment(ec)) {
        for (auto v = std::filesystem::directory_iterator(t->path(), ec);
             !ec && v != std::filesystem::directory_iterator{}; v.increment(ec)) {
            if (auto r = from_text(read_file(v->path() / "specs")); still_there(r))
                return r;
        }
    }

    // The compiler's OWN interpreter.
    //
    // The two sources above are files mcpp used to write, and mcpp no longer
    // writes them -- so on a machine where the toolchain was installed after
    // that change they simply are not there, and the compatibility path that
    // depends on them answers nothing. That is not hypothetical: it is what
    // turned CI red while every existing developer machine, which still has
    // the files from before, stayed green.
    //
    // PT_INTERP is produced by the patchelf walk, which still runs on every
    // install, and it names the glibc payload this toolchain was aligned to --
    // the same fact the specs held, from a mechanism that has not gone away.
    if (auto r = from_text(read_elf_interp(compilerBin)); still_there(r))
        return r;

    // Last: the installed payload set, but ONLY when it is a singleton.
    //
    // This is not the rule this design removed. That rule asked a directory
    // for "the glibc" and took whatever `readdir` yielded first -- a CHOICE,
    // made by something with no bearing on what the artifact would load, and
    // silently wrong the moment a second payload appeared. Where exactly one
    // payload is installed there is no choice to make: it is the only glibc
    // any artifact from this toolchain could bind, and declining would refuse
    // a question that has a single answer.
    //
    // Two or more and this stays silent. That is the case the incident was,
    // and it is the case the subos must answer.
    if (installed.size() == 1) {
        mcpp::log::verbose("probe", std::format(
            "runtime binding glibc@{} — the only glibc payload installed, so "
            "there is nothing to choose between", installed[0]));
        return "glibc@" + installed[0];
    }
    if (installed.size() > 1)
        mcpp::log::verbose("probe", std::format(
            "{} glibc payloads installed and nothing still present declares "
            "which one this build binds; declining rather than picking",
            installed.size()));
    return {};
}


std::filesystem::path write_clean_link_specs(
    const std::filesystem::path& compilerBin,
    const std::filesystem::path& outputDir)
{
    if (compilerBin.empty() || outputDir.empty()) return {};
    std::error_code ec;
    auto out = outputDir / "mcpp-clean-link.specs";
    // Idempotent: the pristine spec is a property of the compiler binary, so
    // regenerating it every build would spawn a process for a constant.
    //
    // `%(sysroot_spec)` IS NOT RESTORED HERE, and that is measured. gcc puts
    // it before `*link:` ahead of any `-specs=` file, so this file drops it
    // and ld hears no `--sysroot` from the driver. For the ecosystem's own
    // view that is what keeps the link working: ld prefixes a sysroot onto the
    // DT_RUNPATH of every shared library it reads, and a SubOS farm library's
    // RUNPATH names the payload store by absolute path (e2e 220: libEGL.so's
    // libGLdispatch, not found). A STATED base hands ld its `--sysroot`
    // explicitly instead (`ToolchainLinkModel::link_tokens`).
    if (std::filesystem::exists(out, ec)) {
        std::ifstream is(out);
        std::string head, body;
        std::getline(is, head);
        std::getline(is, body);
        if (!body.starts_with("%(sysroot_spec) ")) return out;
        // Written by a pre-release build of 2026.10.24.1; regenerated once.
    }

    // An argument vector: the command-string form carried `2>/dev/null`, which
    // cmd.exe cannot open, so on a Windows host this probe printed a spurious
    // path error in every prepared GCC build and never produced the file.
    auto r = mcpp::platform::process::capture_stdout(
        {compilerBin.string(), "-dumpspecs"});
    if (r.exit_code != 0 || r.output.empty()) return {};

    // `*link:` is a section header on its own line; its body is the next line.
    constexpr std::string_view kHead = "*link:\n";
    auto pos = r.output.find(kHead);
    if (pos == std::string::npos) return {};
    auto bodyStart = pos + kHead.size();
    auto bodyEnd   = r.output.find('\n', bodyStart);
    if (bodyEnd == std::string::npos) return {};
    auto body = r.output.substr(bodyStart, bodyEnd - bodyStart);
    if (body.empty()) return {};

    std::filesystem::create_directories(outputDir, ec);
    std::ofstream os(out);
    if (!os) return {};
    os << "*link:\n" << body << "\n\n";
    os.close();
    return out;
}

} // namespace mcpp::toolchain
