// graph_provisioning.cpp -- P4b: the closures a build program's environment is
// filled by (its payloads, its dependencies' directories, its output
// directory, the outputs its actions declare), and the root package. Split
// from graph.cpp at the boundary of the program's environment, when the
// program scopes of resolution (design 2026-11-19 §21 A1) took the file past
// its limit.

module mcpp.build.prepare;
import :state;

import std;
import mcpp.diag;
import mcpp.manifest;
import mcpp.source_kind;
import mcpp.toolchain.fingerprint;
import mcpp.build.build_program;
import mcpp.build.directives;
import mcpp.build.provisions;

namespace mcpp::build {

std::expected<void, std::string>
step4b_define_provisioning_closures(PrepareState& state) {
    // Which dependency supplied the runner, for the exactly-one-provider
    // error below. A name rather than a bool: the message has to name both.
    // ONE PROVIDER PER RUNNER NAME. `runner` has had this rule since #544;
    // a NAMED runner inherits it per name, because a board may legitimately
    // supply `flash` while a different package supplies `monitor`.

    // The payload facts a build program receives: where each declared payload
    // is, and where it came from (mcpp#755). One definition, in sources.cpp,
    // beside the override and on-request rules it reads.
    state.fillXpkgDirs = [&](mcpp::build::BuildProgramEnv& e,
                            const mcpp::manifest::Manifest& owner,
                            std::size_t consumer) {
        fill_xpkg_env(state, e, owner, consumer);
    };

    // `linkForms` (#642 E2): when given, each dependency that has a resolved
    // library form is also offered under exactly the names its directory is,
    // so `dep_linkage(n)` answers for every `n` that `dep_dir(n)` answers for.
    // Only the root's program passes it; see the root call site for why.
    state.fillDepDirs = [&](mcpp::build::BuildProgramEnv& e, std::size_t consumer,
                           const std::map<std::size_t, std::string>* linkForms = nullptr) {
        if (consumer >= state.provisionGraph.visible.size()) return;
        auto bind = state.bareBindingsFor(consumer);
        for (auto const& [tail, b] : bind) {
            if (auto note = prov::contest_note(tail, b); !note.empty())
                mcpp::diag::warning("provisions/ambiguous", note);
        }
        // Every spelling of `publishedNamesFor`: the manifest's name, the
        // qualified name a manifest writing `namespace = "ns"` and
        // `name = "fw"` is addressed by (#642: the framework's rule asks
        // `dep_linkage("huxerui.huxerui")`), and the bound tail. A spelling two
        // providers share (#852: `openmui.rules` and `gongfu.rules` are both
        // `rules` by their manifests' names) is no package's: it is published
        // as ambiguous, with the qualified names that do work.
        std::map<std::string, std::vector<std::size_t>> byName;
        for (auto const& pr : state.provisionGraph.visible[consumer]) {
            if (pr.kind != prov::Kind::DepDir || pr.provider >= state.packages.size()) continue;
            for (auto const& n : state.publishedNamesFor(pr.provider, bind)) {
                auto& who = byName[n];
                if (std::ranges::find(who, pr.provider) == who.end()) who.push_back(pr.provider);
            }
        }
        for (auto const& [n, who] : byName) {
            if (who.size() > 1) {
                std::string which;
                for (auto p : who) {
                    auto const& pk = state.packages[p].manifest.package;
                    const bool qualified = pk.namespace_.empty() || pk.name.starts_with(pk.namespace_ + ".");
                    which += (which.empty() ? "" : ",")
                        + (qualified ? pk.name : pk.namespace_ + "." + pk.name);
                }
                e.depAmbiguous.emplace_back(n, std::move(which));
                continue;
            }
            const auto provider = who.front();
            e.depDirs.emplace_back(n, state.packages[provider].root);
            if (linkForms)
                if (auto f = linkForms->find(provider); f != linkForms->end())
                    e.depLinkages.emplace_back(n, f->second);
        }
    };

    // A declared build-graph node's Source outputs must be visible to the
    // scan, so they are materialized as placeholders and joined to the source
    // set here — the same two lists `generated=` feeds, for the same reason
    // (the scanner walks the legacy modules.sources mirror). ninja overwrites
    // the placeholder before the compile edge runs, because that compile
    // depends on the action's output.
    state.programOutDir = [&state](const std::filesystem::path& base,
                                    const mcpp::manifest::Manifest& pm,
                                    const std::vector<std::string>& features) {
        // What changes the program's environment and is known before it runs.
        std::string id = std::format("target={}\nprofile={}\naccel={}\n",
            state.resolvedTargetCanonical, state.effectiveProfile, state.resolvedAccel());
        if (state.tc)
            id += std::format("toolchain={}@{}\n", state.tc->binaryPath.generic_string(),
                              state.tc->version);
        auto sorted = features;
        std::ranges::sort(sorted);
        for (auto const& f : sorted) id += "feature=" + f + "\n";
        std::string name = pm.package.namespace_.empty()
            ? pm.package.name : pm.package.namespace_ + "." + pm.package.name;
        for (auto& c : name)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-' && c != '_') c = '_';
        return base / "target" / ".build-mcpp" / "out"    // named in `uses.list` (mcpp#874)
             / *state.usedProgramOut.insert(mcpp::toolchain::hash_string(id)).first / (name.empty() ? std::string("root") : name);
    };

    state.adoptActionOutputs = [](mcpp::manifest::Manifest& mm,
                                 const std::filesystem::path& pkgRoot,
                                 std::size_t firstNewAction) {
        if (firstNewAction >= mm.buildConfig.actions.size()) return;
        std::vector<mcpp::manifest::BuildAction> fresh(
            mm.buildConfig.actions.begin()
                + static_cast<std::ptrdiff_t>(firstNewAction),
            mm.buildConfig.actions.end());
        // The package that DECLARED the outputs classifies them: a dependency
        // generating a `.ixx` asks its own manifest, not the root project's.
        // Built once per package, not once per output.
        const auto pkgExtTable =
            mcpp::extension_table_for(mm.buildConfig.moduleExtensions,
                                      mm.buildConfig.deviceExtensions);
        mcpp::build::directives::prepare_actions(fresh, pkgRoot);
        std::copy(fresh.begin(), fresh.end(),
                  mm.buildConfig.actions.begin()
                      + static_cast<std::ptrdiff_t>(firstNewAction));
        for (auto const& a : fresh) {
            if (a.role != mcpp::manifest::BuildAction::Role::Source) continue;
            for (auto const& o : a.outputs) {
                if (o.find("${mcpp.") != std::string::npos) continue;
                // Companion outputs (protoc's .pb.h next to its .pb.cc) are
                // produced by the edge but are NOT translation units.
                if (!mcpp::build::directives::is_compilable_output(o, pkgExtTable))
                    continue;
                mm.buildConfig.sources.push_back(o);
                mm.modules.sources.push_back(o);
                // A declared unit: the scan takes what the action says the
                // file provides and imports, and reads nothing (D6). A unit
                // that declares no `provides` asserts nothing; the build's own
                // scan finds its imports, as it did with the empty placeholder.
                mm.modules.declaredUnits[o] = mcpp::manifest::ScanOverride{a.provides, a.imports, a.origin};
                // D26: a generated module interface must say which module it
                // is. Without `.provides()` nothing writes its BMI, and the
                // first importer fails far from the action.
                if (a.provides.empty()
                    && mcpp::classify(o, pkgExtTable) == mcpp::SourceKind::ModuleInterface)
                    mcpp::diag::degraded("build-program/provides", std::format(
                        "action '{}' generates the module-interface file '{}' and declares no "
                        "`.provides(...)`", a.id, o),
                        "the module it defines has no BMI in this build, so an import of it fails",
                        "declare the module the generated file defines: "
                        "mcpp::action(...).provides(\"<module>\")");
            }
        }
    };


    state.appendUniqueFlags =
        [](std::vector<std::string>& flags,
           const std::vector<std::string>& additions) -> bool
    {
        bool changed = false;
        for (auto const& f : additions) {
            if (std::find(flags.begin(), flags.end(), f) != flags.end()) continue;
            flags.push_back(f);
            changed = true;
        }
        return changed;
    };





    {
        auto rootPackage = makePackageRoot(state, *state.root, *state.m);
        if (!rootPackage) return std::unexpected(rootPackage.error());
        state.packages[0] = std::move(*rootPackage);
    }


    define_usage_requirements(state);

    return {};
}

} // namespace mcpp::build
