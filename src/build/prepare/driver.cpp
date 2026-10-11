// driver.cpp -- prepare_build itself: construct the PrepareState, run the
// phases in order, and return what the last one builds. An early error of any
// phase ends the call with that phase's message.

module mcpp.build.prepare;
import :state;

import mcpp.build.prepare_inputs;

import std;
import mcpp.build.version_floor;
import mcpp.manifest;
import mcpp.source_kind;
// For `resolve_version_match` / `list_installed_versions`: a bare compiler
// family named by the dependency graph resolves to a concrete version through
// exactly the path `mcpp toolchain default <family>` uses.
import mcpp.build.plan;
import mcpp.build.flags;          // compute_flags — the per-role contracts (#418)
import mcpp.build.graph_shape;  // #407: the graph says which mode wrote it
import mcpp.build.build_program;
import mcpp.build.backend;      // BuildOptions for the tool sub-build
import mcpp.build.ninja;        // make_ninja_backend — driving that sub-build
import mcpp.platform;
import mcpp.log;
import mcpp.project;
import mcpp.ui;

namespace mcpp::build {

namespace {
// `thread_local` for the same reason `mcpp::build::refusal`'s sink is
// (refusal.cppm): `prepare_build` recurses for nested host sub-builds on the
// calling thread, and a failure of the INNER call must not leave notes behind
// for an outer call that goes on to succeed. Cleared at the top of every
// `prepare_build` call and on its success path, so only a call that is
// itself failing can leave something here for its caller to take.
thread_local std::vector<PlanNote> g_notesOnFailure;
} // namespace

std::vector<PlanNote> take_notes_on_failure() {
    auto notes = std::move(g_notesOnFailure);
    g_notesOnFailure.clear();
    return notes;
}

namespace {
// `--strict` refuses a deprecated reading the build can edit (docs/51
// §Deprecations): each was warned about where it was read, and is refused
// here, after the phase that reads it, naming every use.
std::expected<void, std::string> refuse_deprecated_readings_under_strict(const PrepareState& state) {
    if (!state.overrides.strict) return {};
    std::string uses;
    for (auto const& u : mcpp::pm::compat::recorded_uses()) {
        if (!u.warned) continue;
        auto const* d = mcpp::pm::compat::find_deprecation(u.id);
        uses += std::format("\n       {}: {} ({}; write {})", u.where, u.detail, u.id,
                            d ? std::string(d->canonical) : std::string("the canonical spelling"));
    }
    if (uses.empty()) return {};
    return std::unexpected("--strict refuses the deprecated readings this build uses:" + uses);
}

std::expected<BuildContext, std::string>
prepare_build_pass(bool print_fingerprint,
                   bool includeDevDeps,
                   std::vector<mcpp::manifest::Target> extraTargets,
                   BuildOverrides overrides);
} // namespace

// TWO PASSES WHEN THE BUILD PROGRAM STATES THE TOOLCHAIN (mcpp#755).
//
// `[toolchain] <key> = { configure = "build.mcpp" }` hands the choice of the
// build toolchain to the root build program. The program needs its host
// modules, and they come from the dependency graph; the graph's resolution
// needs the toolchain (`cfg(compiler = ...)`, `requires`). So the first pass
// prepares with the bootstrap toolchain as far as the host modules, runs the
// program's toolchain phase, and stops; the second prepares with the stated
// toolchain from the start, the bootstrap compiling and running the build
// programs. The first pass narrates nothing: everything it would say, the
// second says about the build that happens.
std::expected<BuildContext, std::string>
prepare_build(bool print_fingerprint,
              bool includeDevDeps,
              std::vector<mcpp::manifest::Target> extraTargets,
              BuildOverrides overrides) {
    if (!overrides.toolchain_statement.empty())
        return prepare_build_pass(print_fingerprint, includeDevDeps,
                                  std::move(extraTargets), std::move(overrides));
    auto first = prepare_build_pass(print_fingerprint, includeDevDeps, extraTargets, overrides);
    if (first) { (void)take_toolchain_restart(); return first; }
    auto restart = take_toolchain_restart();
    if (!restart) return first;
    overrides.toolchain_statement = std::move(restart->first);
    overrides.bootstrap_spec      = std::move(restart->second);
    return prepare_build_pass(print_fingerprint, includeDevDeps,
                              std::move(extraTargets), std::move(overrides));
}

namespace {
// A BUILD PROGRAMS' GRAPH IS SELF-DESCRIBING (mcpp#797). The engine writes
// `.mcpp-build-programs` beside the graph's workspace manifest: the toolchain,
// the profile and the version pins its sub-build was given. A command run in
// that directory reads them, so `mcpp build` there reproduces the compile of
// the build programs exactly, as the build's own error message says it does.
void read_build_programs_marker(BuildOverrides& ov) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto root = ov.project_root;
    if (root.empty())
        if (auto r = mcpp::project::find_manifest_root(fs::current_path(ec))) root = *r;
    if (root.empty()) return;
    std::ifstream in(root / ".mcpp-build-programs");
    if (!in) return;
    ov.build_program_graph = true;
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const auto key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "toolchain" && ov.toolchain.empty()) ov.toolchain = value;
        else if (key == "profile" && ov.profile.empty()) ov.profile = value;
        else if (key == "index-store" && ov.project_index_root.empty()) ov.project_index_root = value;
        else if (key == "pin")
            if (auto p = value.find('='); p != std::string::npos)
                ov.resolution_pins.try_emplace(value.substr(0, p), value.substr(p + 1));
    }
}

std::expected<BuildContext, std::string>
prepare_build_pass(bool print_fingerprint,
                   bool includeDevDeps,
                   std::vector<mcpp::manifest::Target> extraTargets,
                   BuildOverrides overrides) {
    read_build_programs_marker(overrides);
    PrepareState state(print_fingerprint, includeDevDeps,
                        std::move(extraTargets), std::move(overrides));
    pending_flag_words_notes().clear();
    g_notesOnFailure.clear();

    // Every early return below carries `state.planNotes` as they stood at the
    // failing phase, so a caller whose only handle on the failure is
    // `.error()` (a plain string) can still read what an earlier phase
    // recorded — see `take_notes_on_failure`'s declaration in prepare.cppm.
    auto fail = [&](std::string message) -> std::unexpected<std::string> {
        g_notesOnFailure = state.planNotes;
        return std::unexpected(std::move(message));
    };

    // WHERE PLANNING'S TIME GOES: each phase states its duration under
    // `build/stage` in the log file, which --verbose or MCPP_LOG_LEVEL=info
    // enables. Planning had no such record, and a planned edit of one source
    // spent 3.05 s before ninja that could only be attributed from gaps
    // between unrelated log lines (.agents/docs/
    // 2026-09-30-build-wall-time-progress-count-and-hang-plan.md, W9). The
    // file and not the terminal: thirty lines a build are a record to read
    // afterwards, not output, and their words would reach every check that
    // reads what a verbose build says.
    auto timed = [&](std::string_view phase, auto&& run) {
        if (!mcpp::log::is_enabled(mcpp::log::Level::info)) return run();
        const auto t0 = std::chrono::steady_clock::now();
        auto r = run();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        mcpp::log::info("build/stage", std::format("plan {}: {}ms", phase, ms));
        return r;
    };

    if (auto r = timed("manifest", [&] { return phase0_manifest_and_workspace(state); }); !r)
        return fail(r.error());
    if (auto r = check_engine_floors(state, /*rootOnly=*/true); !r) return fail(r.error());
    if (auto r = timed("toolchain request", [&] { return phase1_toolchain_spec_and_axes(state); }); !r)
        return fail(r.error());
    // The first pass of a toolchain phase says nothing (see prepare_build).
    struct QuietPass {
        bool active = false, prev = false;
        ~QuietPass() { if (active) mcpp::ui::set_quiet(prev); }
    } quietPass;
    if (state.toolchainConfigure && state.overrides.toolchain_statement.empty()) {
        quietPass.active = true;
        quietPass.prev   = mcpp::ui::is_quiet();
        mcpp::ui::set_quiet(true);
    }
    if (auto r = timed("toolchain resolver", [&] { return phase2_define_toolchain_resolver(state); }); !r)
        return fail(r.error());
    if (auto r = timed("xlings", [&] { return phase3_xlings_before_graph(state); }); !r)
        return fail(r.error());
    if (auto r = timed("graph load", [&] { return phase4a_graph_load(state); }); !r)
        return fail(r.error());
    if (auto r = timed("graph", [&] { return phase4b_graph_worklist(state); }); !r)
        return fail(r.error());
    if (auto r = check_engine_floors(state, /*rootOnly=*/false); !r) return fail(r.error());
    if (auto r = refuse_deprecated_readings_under_strict(state); !r) return fail(r.error());
    if (auto r = timed("toolchain", [&] { return phase5_toolchain_after_graph(state); }); !r)
        return fail(r.error());
    if (auto r = timed("features and host tools", [&] { return phase6_features_and_host_tools(state); }); !r)
        return fail(r.error());
    if (auto r = timed("target side", [&] { return phase9_target_side(state); }); !r)
        return fail(r.error());
    if (auto r = timed("scan", [&] { return phase11_scan(state); }); !r)
        return fail(r.error());
    if (auto r = refuse_deprecated_readings_under_strict(state); !r) return fail(r.error());

    g_notesOnFailure.clear();
    return timed("finish", [&] { return phase13_finish(state); });
}
} // namespace


} // namespace mcpp::build
