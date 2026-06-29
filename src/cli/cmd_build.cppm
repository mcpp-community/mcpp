// mcpp.cli.cmd_build — CLI parsing + routing for build / run / test /
// clean / dyndep. Implementations live in mcpp.build.prepare,
// mcpp.build.execute and mcpp.dyndep.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.cli.cmd_build;

import std;
import mcpplibs.cmdline;
import mcpp.build.prepare;
import mcpp.build.execute;
import mcpp.dyndep;
import mcpp.log;
import mcpp.project;

namespace mcpp::cli {

export int cmd_build(const mcpplibs::cmdline::ParsedArgs& parsed) {
    bool verbose  = parsed.is_flag_set("verbose") || mcpp::log::is_verbose();
    bool print_fp = parsed.is_flag_set("print-fingerprint");
    bool no_cache = parsed.is_flag_set("no-cache");

    mcpp::build::BuildOverrides ov;
    if (auto t = parsed.value("target")) ov.target_triple = *t;
    if (auto p = parsed.value("package")) ov.package_filter = *p;
    if (auto pr = parsed.value("profile")) ov.profile = *pr;
    if (auto fs = parsed.value("features")) ov.features = *fs;
    if (auto cp = parsed.value("cap")) ov.capabilities = *cp;
    ov.strict = parsed.is_flag_set("strict");
    ov.force_static = parsed.is_flag_set("static");

    // P0: try fast-path if inputs haven't changed. Any resolution-affecting
    // override (--profile/--features/--strict, like --target/--static) must
    // bypass it: the cached build.ninja was generated without them, so taking
    // the fast path would silently ignore the flags.
    if (!print_fp && ov.target_triple.empty() && !ov.force_static
        && ov.profile.empty() && ov.features.empty() && !ov.strict
        && ov.capabilities.empty() && ov.package_filter.empty()) {
        auto root = mcpp::project::find_manifest_root(std::filesystem::current_path());
        if (root) {
            if (auto rc = mcpp::build::try_fast_build(*root, verbose, no_cache)) {
                return *rc;
            }
        }
    }

    auto ctx = mcpp::build::prepare_build(print_fp, /*includeDevDeps=*/false,
                                          /*extraTargets=*/{}, ov);
    if (!ctx) { std::println(stderr, "error: {}", ctx.error()); return 2; }

    return mcpp::build::run_build_plan(*ctx, verbose, no_cache, ov.target_triple);
}

export int cmd_run(const mcpplibs::cmdline::ParsedArgs& parsed,
            std::span<const std::string> passthrough) {
    // The action lambda has already split argv at the first "--" and
    // passed post-args as `passthrough`; the only positional we declare
    // is the optional binary target name.
    std::optional<std::string> targetName;
    if (parsed.positional_count() > 0) targetName = parsed.positional(0);
    return mcpp::build::build_run_target(targetName, passthrough);
}

export int cmd_test(const mcpplibs::cmdline::ParsedArgs& parsed,
             std::span<const std::string> passthrough) {
    // Pre-`--` flags select the build mode for the test build (so e.g.
    // `mcpp test --profile contracts` compiles the code-under-test plus the
    // test binaries under that profile — a whole-build mode, the right
    // granularity for sanitizers / contract evaluation semantics). Post-`--`
    // args go to each test binary.
    mcpp::build::BuildOverrides ov;
    if (auto pr = parsed.value("profile"))  ov.profile  = *pr;
    if (auto fs = parsed.value("features")) ov.features = *fs;
    if (auto cp = parsed.value("cap")) ov.capabilities = *cp;
    ov.strict = parsed.is_flag_set("strict");
    return mcpp::build::run_tests(passthrough, ov);
}

export int cmd_clean(const mcpplibs::cmdline::ParsedArgs& parsed) {
    return mcpp::build::clean_project(parsed.is_flag_set("bmi-cache"));
}

// Hidden subcommand: aggregate P1689 .ddi files into a Ninja dyndep file.
// Invoked by ninja during build (cxx_collect / cxx_dyndep rules).
//
// Multi-file mode (legacy cxx_collect):
//   mcpp dyndep --output <build.ninja.dd> <ddi-1> <ddi-2> ...
//
// Single-file mode (P1 per-file dyndep, cxx_dyndep rule):
//   mcpp dyndep --single --output <file.dd> <file.ddi>
export int cmd_dyndep(const mcpplibs::cmdline::ParsedArgs& parsed) {
    std::filesystem::path outPath = parsed.option_or_empty("output").value();
    if (outPath.empty()) {
        std::println(stderr, "error: --output <path> required");
        return 2;
    }

    bool single = parsed.is_flag_set("single");

    mcpp::dyndep::DyndepOptions opts;
    std::string bmiDirStorage = parsed.option_or_empty("bmi-dir").value();
    std::string bmiExtStorage = parsed.option_or_empty("bmi-ext").value();
    if (!bmiDirStorage.empty())
        opts.bmiDir = bmiDirStorage;
    if (!bmiExtStorage.empty())
        opts.bmiExt = bmiExtStorage;

    std::expected<std::string, std::string> body;
    if (single) {
        if (parsed.positional_count() != 1) {
            std::println(stderr, "error: --single requires exactly one .ddi input");
            return 2;
        }
        body = mcpp::dyndep::emit_dyndep_single(parsed.positional(0), opts);
    } else {
        std::vector<std::filesystem::path> ddis;
        for (std::size_t i = 0; i < parsed.positional_count(); ++i)
            ddis.emplace_back(parsed.positional(i));
        body = mcpp::dyndep::emit_dyndep_from_files(ddis, /*stdImports=*/{}, opts);
    }

    if (!body) {
        std::println(stderr, "error: {}", body.error());
        return 1;
    }
    std::error_code ec;
    std::filesystem::create_directories(outPath.parent_path(), ec);
    std::ofstream os(outPath);
    os << *body;
    return os ? 0 : 1;
}

} // namespace mcpp::cli
