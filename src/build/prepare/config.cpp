// config.cpp -- the manifest rules the phases apply: conditional
// `[target.*]` and `[xlings]` merges, build flags and defines, workspace
// inheritance, feature requests and std-module detection. Moved verbatim from
// the former preamble of prepare.cppm; declared in `:state`, or exported from
// prepare.cppm where the interface or the unit tests need them.

module mcpp.build.prepare;
import :state;

import mcpp.build.prepare_inputs;

import std;
import mcpp.targetside;
import mcpp.diag;
import mcpp.xlings.address_set;
import mcpp.build.version_floor;
import mcpp.platform.axis;
import mcpp.manifest;
import mcpp.source_kind;
import mcpp.modgraph.glob;
import mcpp.modgraph.graph;
import mcpp.modgraph.scanner;
import mcpp.modgraph.validate;
// For `resolve_version_match` / `list_installed_versions`: a bare compiler
// family named by the dependency graph resolves to a concrete version through
// exactly the path `mcpp toolchain default <family>` uses.
import mcpp.build.plan;
import mcpp.build.flags;          // compute_flags — the per-role contracts (#418)
import mcpp.build.graph_shape;  // #407: the graph says which mode wrote it
import mcpp.pack.abi_tag;      // the tag a prebuilt dependency is checked against
import mcpp.pack.prebuilt;     // …and the check itself
import mcpp.pack.stage_tree;   // where `${mcpp.stage_dir}` points, and its manifest
import mcpp.build.build_program;
import mcpp.build.backend;      // BuildOptions for the tool sub-build
import mcpp.build.ninja;        // make_ninja_backend — driving that sub-build
import mcpp.xlings;
import mcpp.platform;
import mcpp.ui;
import mcpp.project;

namespace mcpp::build {

// mcpp#237: surface xpkg-descriptor mcpp-segment keys this mcpp did not
// recognise. The parser collects them into `xpkgUnknownKeys` and skips the
// value; without this a typo like `dependencies = {...}` (correct key: `deps`)
// dropped the dependency with no diagnostic. Called at the descriptor-adoption
// sites (a fetched dep with no mcpp.toml, synthesized from the index `mcpp={}`
// block) — the single place the descriptor becomes a build input. Warning (not
// hard error) keeps forward-compat: an older mcpp building a newer descriptor
// should not fail outright, only tell the user what it ignored.
void warn_unknown_xpkg_keys(const mcpp::manifest::Manifest& dm,
                                   std::string_view depLabel) {
    // A LAYER NAME THIS ENGINE DOES NOT KNOW IS A VERSION GAP, NOT A TYPO,
    // WHEN IT ARRIVES FROM A DEPENDENCY.
    //
    // The reserved `mcpp:` prefix is a closed set so a misspelling cannot
    // silently disable a behaviour. Refusing a DEPENDENCY's manifest for it made
    // the set closed in a second sense nobody intended: a published package
    // could never declare a layer named after the reader was released.
    // Ignoring the layer and saying so is what this engine already does for
    // every other unknown key, and it is the only response that lets the
    // vocabulary grow.
    for (auto const& cap : dm.unknownCapabilities) {
        auto why = mcpp::targetside::parse_capability(cap);
        mcpp::ui::warning(std::format(
            "dependency '{}': {}\n"
            "       Ignored, and this build proceeds without that layer. "
            "A newer mcpp may resolve it.",
            depLabel,
            why ? std::format("`{}` names no capability mcpp knows.", cap)
                : why.error()));
    }
    for (auto const& key : dm.xpkgUnknownKeys) {
        auto suggestion = mcpp::manifest::closest_known_xpkg_key(key);
        if (suggestion.empty())
            mcpp::ui::warning(std::format(
                "dependency '{}': unknown mcpp-segment key '{}' in its xpkg "
                "descriptor — ignored (schema mismatch or typo)", depLabel, key));
        else
            mcpp::ui::warning(std::format(
                "dependency '{}': unknown mcpp-segment key '{}' in its xpkg "
                "descriptor — ignored; did you mean '{}'?", depLabel, key, suggestion));
    }
}

// `stale`, when given, turns the function into a comparison: nothing is
// created or written, and every declared file that is missing or differs from
// its declared content is appended. A build that describes itself rather than
// running (BuildOverrides::plan_only) reads the root package's generated files
// this way, because they live in the source tree it promises not to write.
std::expected<void, std::string>
materialize_generated_files(const std::filesystem::path& root,
                            const mcpp::manifest::Manifest& manifest,
                            std::vector<std::filesystem::path>* stale) {
    for (auto const& [relPath, content] : manifest.buildConfig.generatedFiles) {
        if (relPath.empty()) {
            return std::unexpected("generated_files contains an empty path");
        }
        if (relPath.is_absolute()) {
            return std::unexpected(std::format(
                "generated_files path '{}' must be relative", relPath.generic_string()));
        }
        auto const genericPath = relPath.generic_string();
        for (std::size_t begin = 0; begin <= genericPath.size();) {
            auto const end = genericPath.find('/', begin);
            auto const part = genericPath.substr(begin, end == std::string::npos
                                                           ? std::string::npos
                                                           : end - begin);
            if (part == "..") {
                return std::unexpected(std::format(
                    "generated_files path '{}' must not escape the package root",
                    relPath.generic_string()));
            }
            if (end == std::string::npos) {
                break;
            }
            begin = end + 1;
        }

        auto out = root / relPath.lexically_normal();

        // Skip the write when the on-disk content is already identical: ninja
        // is mtime-driven, and an unconditional rewrite bumps the mtime every
        // build, recompiling every TU that #includes the materialized file
        // (via depfiles) — e.g. a frozen-snapshot config.h included by
        // thousands of TUs. Change detection is already owned by the
        // fingerprint (content is folded in above), so skipping only
        // preserves the mtime — mirroring the build.mcpp cache design,
        // which likewise avoids mtime churn on unchanged outputs.
        {
            std::ifstream is(out, std::ios::binary);
            if (is) {
                std::string existing((std::istreambuf_iterator<char>(is)),
                                     std::istreambuf_iterator<char>());
                if (is && existing == content) {
                    continue;
                }
            }
        }
        if (stale) {
            stale->push_back(out);
            continue;
        }

        std::error_code ec;
        std::filesystem::create_directories(out.parent_path(), ec);
        if (ec) {
            return std::unexpected(std::format(
                "cannot create directory for generated file '{}': {}",
                out.string(), ec.message()));
        }
        std::ofstream os(out, std::ios::binary);
        if (!os) {
            return std::unexpected(std::format(
                "cannot write generated file '{}'", out.string()));
        }
        os << content;
        if (!os) {
            return std::unexpected(std::format(
                "failed while writing generated file '{}'", out.string()));
        }
    }
    return {};
}

void merge_conditional_xlings(mcpp::manifest::Manifest& m,
                                     const mcpp::manifest::ConditionalConfig& cc) {
    // ONE DEFINITION OF IDENTITY, and it is not local to this merge. It used
    // to be `parse_address(a).target` — the bare name, so `xim:cuda` and a
    // hypothetical `scode:cuda` collided, and the graph split a few thousand
    // lines below compared whole address strings instead. See
    // mcpp.xlings.address_set for what the two definitions cost.
    auto package_of = [](std::string_view address) {
        return mcpp::xlings::addrset::package_key(address);
    };
    for (auto const& a : cc.xlings.deps) {
        const auto pkg = package_of(a);
        auto it = std::ranges::find_if(m.xlings.deps, [&](const std::string& e) {
            return package_of(e) == pkg;
        });
        if (it == m.xlings.deps.end()) { m.xlings.deps.push_back(a); continue; }
        if (*it != a)
            mcpp::diag::warning("xlings/axis-override", std::format(
                "'{}' is declared on both tool axes, as '{}' and as '{}'. The "
                "[target.<selector>] entry is the more specific statement and "
                "is the one used. Declare a tool that runs on the build machine "
                "in the top-level [xlings.workspace], and what the produced "
                "code is compiled against under [target.<selector>.xlings."
                "workspace] — see docs/05 section 2.13.", pkg, *it, a));
        *it = a;
    }
    // Keyed by PACKAGE, so the same override applies without a second search.
    for (auto const& [pkg, pin] : cc.xlings.workspace)
        m.xlings.workspace.insert_or_assign(pkg, pin);
    // Keyed by ADDRESS. `insert_or_assign` rather than `try_emplace` for the
    // same reason: the address that survived above is the conditional one.
    for (auto const& [addr, w] : cc.xlings.depWhen)
        m.xlings.depWhen.insert_or_assign(addr, w);
    m.xlings.onRequest.insert(cc.xlings.onRequest.begin(), cc.xlings.onRequest.end());
    // An override written under a selector is the more specific statement of
    // where this package comes from for that target, so it replaces the
    // top-level one for the same package (mcpp#755).
    for (auto const& [pkg, o] : cc.xlings.overrides)
        m.xlings.overrides.insert_or_assign(pkg, o);
    for (auto const& [f, addrs] : cc.xlings.featureDeps) {
        auto& dst = m.xlings.featureDeps[f];
        for (auto const& a : addrs) {
            const auto pkg = package_of(a);
            auto it = std::ranges::find_if(dst, [&](const std::string& e) {
                return package_of(e) == pkg;
            });
            if (it == dst.end()) dst.push_back(a); else *it = a;
        }
    }
    for (auto const& [addr, pin] : cc.xlings.featurePins)
        m.xlings.featurePins.insert_or_assign(addr, pin);
}

std::optional<std::string>
layer_predicated_xlings_refusal(const mcpp::manifest::Manifest& m) {
    for (auto const& cc : m.conditionalConfigs) {
        if (cc.xlings.empty()) continue;
        if (!cfgpred::uses_layer(cc.predicate)) continue;
        std::string named;
        for (auto const& a : cc.xlings.deps) {
            if (!named.empty()) named += ", ";
            named += a;
        }
        for (auto const& [f, addrs] : cc.xlings.featureDeps)
            for (auto const& a : addrs) {
                if (!named.empty()) named += ", ";
                named += std::format("{} (feature '{}')", a, f);
            }
        return std::format(
            "[target.'{}'] declares tools ({}), but its predicate names a "
            "target-side layer. A layer is answered by dependency resolution, "
            "which happens after tools are installed and after build programs "
            "run, so a tool conditioned on one would be declared and never "
            "installed. Condition it on the target instead "
            "(`[target.'cfg(os = \"linux\")'.xlings.workspace]`), on the "
            "accelerator (`[target.'cfg(accelerator = \"cuda\")'.xlings"
            ".workspace]`, which IS answered before provisioning), or on a "
            "feature (`[feature-xlings.<feature>]`). See docs/05 section 2.13.",
            cc.predicate, named);
    }
    return std::nullopt;
}

// Two declarations of one dependency, compared by the identity their keys
// normalise to rather than by the keys themselves: `fw` and `mcpplibs.fw` are
// one package under two map keys (`selector.stableMapKey`), and a comparison
// of keys would leave both entries in the map for the resolver to see.
bool same_dependency_identity(const mcpp::manifest::DependencySpec& a,
                              const mcpp::manifest::DependencySpec& b) {
    if (a.shortName.empty() || b.shortName.empty()) return false;
    return a.namespace_ == b.namespace_ && a.shortName == b.shortName;
}

void replace_dependencies(
    std::map<std::string, mcpp::manifest::DependencySpec>& into,
    const std::map<std::string, mcpp::manifest::DependencySpec>& from) {
    for (auto const& [key, spec] : from) {
        std::erase_if(into, [&](auto const& entry) {
            return entry.first == key || same_dependency_identity(entry.second, spec);
        });
        into[key] = spec;
    }
}

void merge_conditional_config(mcpp::manifest::Manifest& m,
                                    const cfgpred::Ctx& ctx) {
    // Recorded before the first merge; see Manifest::beforeConditionalMerge.
    if (!m.beforeConditionalMerge)
        m.beforeConditionalMerge = std::make_shared<const mcpp::manifest::Manifest>(m);
    // A DISTRIBUTION package may carry a leg's link line twice: as `ldflags`
    // (GNU spelling, which is all an older mcpp reads) and as the neutral
    // `[target.<pred>.runtime]` pair, which mcpp renders per dialect. Applying
    // both would put `-L` on a native `cl.exe` command line, which is exactly
    // what the neutral form exists to avoid — so where the neutral form is
    // present it REPLACES the ldflags rather than adding to them.
    //
    // Scoped to distribution packages on purpose: a hand-written manifest that
    // states both may well mean both (`ldflags` also carries things like
    // `-Wl,--as-needed`), and silently dropping half of it would be its own
    // silent failure.
    const bool generatedPackage = mcpp::pack::is_distribution_package(m);

    for (auto const& cc : m.conditionalConfigs) {
        // THE TWO PASSES MUST BE DISJOINT, AND `matches()` ALONE DOES NOT
        // MAKE THEM SO. A layer key answers false here because `layersKnown` is
        // false — but `cfg(any(linux, c-abi = "musl"))` still matches on its
        // triple leg, and the second pass would match it again and `append()`
        // the same inputs twice. Membership, not the answer, decides ownership:
        // a predicate that NAMES a layer belongs to the second pass entirely.
        if (cfgpred::uses_layer(cc.predicate)) continue;
        if (!cfgpred::matches(cc.predicate, ctx)) continue;
        const bool neutralWins = generatedPackage
                              && (!cc.linkLibraryDirs.empty() || !cc.libraries.empty()
                                  || !cc.frameworks.empty());
        // One append() for every field the axis may carry (#258). Matching
        // sections land AFTER the base entries, so a conditional rule beats
        // a broader unconditional one under GNU last-wins — which is what
        // makes an off-OS REMOVAL expressible (`-U` after the base `-D`).
        if (neutralWins) {
            // Drop the LIBRARY REFERENCES, not the whole ldflags list.
            //
            // Clearing it outright was a measured regression: a PE/MinGW shared
            // leg's ldflags also carry `-Wl,-Bdynamic`, without which `-static`
            // leaves ld in static-only mode and it refuses the import library
            // with `have you installed the static version of the mathkit
            // library?`. e2e 257 caught it.
            //
            // The neutral form replaces exactly what it can express — a library
            // and where to find it. Anything else in that block says something
            // it cannot say, and must survive.
            auto inputs = cc.inputs;
            std::erase_if(inputs.ldflags, [](std::string_view f) {
                return f.starts_with("-L") || f.starts_with("-l")
                    || f.starts_with("/LIBPATH:");
            });
            mcpp::manifest::append(m.buildConfig, inputs);
        } else {
            mcpp::manifest::append(m.buildConfig, cc.inputs);
        }
        // The neutral half goes where `render_link_intent_flags` will find it.
        for (auto const& d : cc.linkLibraryDirs)
            m.runtimeConfig.linkIntent.linkLibraryDirs.push_back(d);
        for (auto const& l : cc.libraries)
            m.runtimeConfig.linkIntent.libraries.push_back(l);
        for (auto const& f : cc.frameworks)
            m.runtimeConfig.linkIntent.frameworks.push_back(f);
        merge_conditional_xlings(m, cc);
        // `[target.<sel>.abi]`: recorded for every package; rendered only for
        // the root, where prepare_build reads it. Last matching section wins,
        // the rule every other conditional scalar follows.
        if (cc.abiThreadsDeclared) {
            m.buildConfig.abiThreads = cc.abiThreads;
            m.buildConfig.abiThreadsDeclared = true;
        }
        if (cc.abiExceptionsDeclared) {
            m.buildConfig.abiExceptions = cc.abiExceptions;
            m.buildConfig.abiExceptionsDeclared = true;
        }
        // `[target.<sel>.build] dialect_cxxflags` (#717): recorded for every
        // package, like the abi switches above, but APPENDED rather than
        // replaced -- there is no "last matching section wins" here, because
        // the key is additive by design (design 2026-09-27 §6: "entries are
        // appended, as cxxflags are"). This is the SAME iteration this loop
        // already performs in manifest order, so a package's matching rows
        // land after its own unconditional `[build] dialect_cxxflags`
        // (already in `m.buildConfig.dialectCxxflags` from the initial parse)
        // in exactly the declared order. Only the root's resulting list is
        // ever read downstream; a dependency's is inert on that dependency's
        // own manifest and excluded from its fingerprint contribution
        // (prepare_inputs.cppm).
        if (!cc.dialectCxxflags.empty())
            m.buildConfig.dialectCxxflags.insert(m.buildConfig.dialectCxxflags.end(),
                                                 cc.dialectCxxflags.begin(),
                                                 cc.dialectCxxflags.end());
        // `[target.<sel>] requires_abi` / `.feature-requires-abi` (A6): a
        // requirement on the TARGET axis, unioned in -- not overwritten --
        // because more than one matching selector may ask for the same
        // member, and every one of them is a true statement. The selector
        // text rides along so the unmet-requirement check can name what
        // asked, the same courtesy `[package] requires_abi` gets by naming
        // "the package" and a feature's entry by naming the feature.
        if (cc.requiresAbiThreads)
            m.targetRequiresAbiThreads.push_back(cc.predicate);
        if (cc.requiresAbiExceptions)
            m.targetRequiresAbiExceptions.push_back(cc.predicate);
        for (auto const& [f, val] : cc.featureRequiresAbiThreads)
            if (val) m.targetFeatureRequiresAbiThreads[f].push_back(cc.predicate);
        for (auto const& [f, val] : cc.featureRequiresAbiExceptions)
            if (val) m.targetFeatureRequiresAbiExceptions[f].push_back(cc.predicate);
        // `modules.sources` is the scanner's own view and is not part of
        // BuildInputs, so conditional sources are mirrored into it here.
        for (auto const& s : cc.inputs.sources)
            m.modules.sources.push_back(s);
        // A matching conditional declaration of a dependency REPLACES the
        // declaration of the same identity, and a later matching section
        // replaces an earlier one: the rule every conditional scalar above
        // follows (#634, A1). This used to be `insert()`, which kept the
        // unconditional entry, so `linkage = "shared"` written for one row was
        // dropped on that row without a word. No manifest among 509 scanned
        // declared one dependency in both tables, so no build that worked
        // changes; the declaring table rides on the spec (`declaredIn`) into
        // the resolution record.
        replace_dependencies(m.dependencies, cc.dependencies);
        replace_dependencies(m.devDependencies, cc.devDependencies);
        replace_dependencies(m.buildDependencies, cc.buildDependencies);
        // #359: `[target.<sel>.feature-deps.<feature>]`. The feature is
        // registered by the parser regardless of the predicate; only what it
        // pulls in is conditional.
        for (auto const& [fname, deps] : cc.featureDeps)
            replace_dependencies(m.featureDeps[fname], deps);
        // `[target.<sel>.targets.<name>] kind`: the row's form of a library
        // target, applied before resolution, so the link-form resolution
        // reads it exactly as it reads `[targets.<name>] kind`. `load` has
        // already refused a name that is not a library target.
        //
        // `linkage` (#642 E1) is the row's default form, and a row's statement
        // REPLACES the statement it follows, whichever of the two each one is:
        // a default after `kind = "shared"` returns the target to the library
        // form a consumer may choose from, and a `kind` after a default clears
        // the default. Last matching section wins, as for every conditional
        // scalar.
        for (auto const& [name, row] : cc.targetKinds) {
            for (auto& t : m.targets) {
                if (t.name != name) continue;
                t.kindDeclaredBy = row.statement;
                t.kindFromRow = true;
                if (!row.linkage.empty()) {
                    t.kind = mcpp::manifest::Target::Library;
                    t.linkageDefault = row.linkage;
                    t.linkageDeclaredBy = row.statement;
                } else {
                    t.kind = row.kind;
                    t.linkageDefault.clear();
                    t.linkageDeclaredBy.clear();
                }
            }
        }
    }
}

// ── An element whose words changed in 2026.9.17.1 (#655) ─────────────────────
//
// A compile-flag element used to reach the compiler as its host's command-line
// reader made it (POSIX `sh`, or the MSVCRT rules), after ninja had replaced
// `$` sequences, with a `-D` element containing a space quoted whole (#234).
// It now reaches the compiler as `flag_words` reads it, and a `defines` value
// is one word. Most spellings mean the same under both readings; the ones that
// do not are told what the compiler receives now and what it received before.
// The previous reading is modelled on quote removal only: `sh` expansions
// (`$VAR`, globs) are not reproduced.
//
// WHEN IT IS SAID. The notes are collected while manifests load and released
// where the output directory is decided, only if that directory has no
// build.ninja yet. The fingerprint names the mcpp version and every flag, so
// that is the first plan after an upgrade, after a flag was edited, or in a
// fresh checkout. A build that repeats a plan says nothing, so a manifest that
// is already spelled for the new reading is not warned about on every run.
std::vector<std::pair<std::string, std::string>>& pending_flag_words_notes() {
    static std::vector<std::pair<std::string, std::string>> notes;
    return notes;
}

std::vector<std::string> previous_release_words(std::string element, bool define) {
    if (define) element = "-D" + element;
    if ((element.starts_with("-D") || element.starts_with("/D"))
        && element.find(' ') != std::string::npos)
        element = mcpp::build::shell_quote_arg(element);
    std::string line;
    for (std::size_t i = 0; i < element.size(); ++i) {
        if (element[i] != '$' || i + 1 == element.size()) { line.push_back(element[i]); continue; }
        const char n = element[i + 1];
        if (n == '$' || n == ' ' || n == ':') { line.push_back(n); ++i; continue; }
        // A ninja variable reference: `${name}` or `$name`, empty on a compile edge.
        std::size_t j = i + 1;
        if (n == '{') {
            while (j < element.size() && element[j] != '}') ++j;
        } else {
            while (j + 1 < element.size()
                   && (std::isalnum(static_cast<unsigned char>(element[j + 1]))
                       || element[j + 1] == '_' || element[j + 1] == '-'))
                ++j;
        }
        i = j;
    }
    return mcpp::manifest::host_command_words(line, mcpp::platform::is_windows);
}

void report_flag_words_changes(const mcpp::manifest::Manifest& m) {
    auto show = [](const std::vector<std::string>& words) {
        std::string out = "[";
        for (auto const& w : words)
            out += std::format("{}'{}'", out.size() > 1 ? ", " : "", w);
        return out + "]";
    };
    auto note_change = [&](std::string what, std::string hint) {
        auto note = std::pair{std::move(what), std::move(hint)};
        auto& notes = pending_flag_words_notes();
        if (std::ranges::find(notes, note) == notes.end()) notes.push_back(std::move(note));
    };
    auto const who = m.package.name.empty() ? std::string("(root)") : m.package.name;
    auto check = [&](std::string_view where, const std::vector<std::string>& list,
                     bool define) {
        for (auto const& e : list) {
            auto now = define ? std::vector<std::string>{"-D" + e}
                              : mcpp::manifest::flag_words(e);
            auto before = previous_release_words(e, define);
            if (now == before) continue;
            note_change(std::format(
                "{}: {} element '{}' reaches the compiler as {}; mcpp before "
                "2026.9.17.1 passed {} on this host",
                who, where, e, show(now), show(before)),
                std::string(
                "a compile-flag element is read by one syntax on every host, and a "
                "`defines` entry is one value (docs/04-mcpp-toml.md, "
                "\"Compile-flag syntax\"); spell the element so that it reads as the "
                "words meant"));
        }
    };
    // THE SAME QUESTION FOR THE LINK FLAGS, which take the reading from
    // 2026.9.26.2 (#703). Before, a `-L` or `-Wl,-rpath,` element was escaped
    // for ninja, so its text reached the host's reader as written, and any
    // other element was pasted into the ninja rule, so ninja replaced its `$`
    // sequences first. `$ORIGIN` written plainly reads the same under both
    // models, because neither reproduces the shell's expansion that lost it;
    // an element escaped for ninja or for the shell by hand is what differs.
    auto check_link = [&](std::string_view where, const std::vector<std::string>& list) {
        for (auto const& e : list) {
            auto now = mcpp::manifest::flag_words(e);
            auto before = e.starts_with("-L") || e.starts_with("-Wl,-rpath,")
                ? mcpp::manifest::host_command_words(e, mcpp::platform::is_windows)
                : previous_release_words(e, false);
            if (now == before) continue;
            note_change(std::format(
                "{}: {} element '{}' reaches the linker as {}; mcpp before "
                "2026.9.26.2 passed {} on this host",
                who, where, e, show(now), show(before)),
                std::string(
                "a link-flag element is read by the compile-flag syntax, so `$ORIGIN` "
                "reaches the linker as written and an element escaped for ninja or the "
                "shell by hand is no longer unescaped (docs/04-mcpp-toml.md, "
                "\"Compile-flag syntax\"); spell the element so that it reads as the "
                "words meant"));
        }
    };
    auto const& bc = m.buildConfig;
    check_link("[build] ldflags", bc.ldflags);
    check("[build] cflags", bc.cflags, false);
    check("[build] cxxflags", bc.cxxflags, false);
    check("[build] defines", bc.defines, true);
    for (auto const& gf : bc.globFlags) {
        check("flags cflags", gf.cflags, false);
        check("flags cxxflags", gf.cxxflags, false);
        check("flags asmflags", gf.asmflags, false);
        check("flags defines", gf.defines, true);
    }
    for (auto const& [feature, defines] : bc.featureDefines)
        check(std::format("features.{} defines", feature), defines, true);
    for (auto const& [feature, globs] : bc.featureFlags) {
        for (auto const& gf : globs) {
            check(std::format("features.{} cflags", feature), gf.cflags, false);
            check(std::format("features.{} cxxflags", feature), gf.cxxflags, false);
            check(std::format("features.{} asmflags", feature), gf.asmflags, false);
            check(std::format("features.{} defines", feature), gf.defines, true);
        }
    }
    for (auto const& t : m.targets) {
        check(std::format("targets.{} cflags", t.name), t.cflags, false);
        check(std::format("targets.{} cxxflags", t.name), t.cxxflags, false);
        check(std::format("targets.{} defines", t.name), t.defines, true);
    }
}

// The macro name a `defines` entry or a `-D` word defines: the text before
// the first `=`, or the whole text when there is no value.
std::string_view define_name(std::string_view entry) {
    return entry.substr(0, entry.find('='));
}

void fold_build_defines_into_flags(mcpp::manifest::BuildConfig& bc) {
    if (bc.defines.empty()) return;

    std::vector<std::string> resolved;          // entries, first-seen order
    std::vector<std::string> named;             // every name this call touches
    auto touch = [&](std::string_view name) {
        if (std::ranges::find(named, name) == named.end())
            named.emplace_back(name);
    };
    for (auto const& d : bc.defines) {
        if (d.starts_with('!')) {
            const auto name = std::string_view(d).substr(1);
            std::erase_if(resolved, [&](const std::string& e) {
                return define_name(e) == name;
            });
            touch(name);
            continue;
        }
        const auto name = define_name(d);
        touch(name);
        auto it = std::ranges::find_if(resolved, [&](const std::string& e) {
            return define_name(e) == name;
        });
        if (it != resolved.end()) *it = d;
        else resolved.push_back(d);
    }

    auto superseded = [&](const std::string& element) {
        auto words = mcpp::manifest::flag_words(element);
        if (words.size() != 1 || !words.front().starts_with("-D")) return false;
        const auto name = define_name(std::string_view(words.front()).substr(2));
        return std::ranges::find(named, name) != named.end();
    };
    std::erase_if(bc.cflags, superseded);
    std::erase_if(bc.cxxflags, superseded);

    for (auto const& d : resolved) {
        const auto element = mcpp::manifest::flag_element("-D" + d);
        bc.cflags.push_back(element);
        bc.cxxflags.push_back(element);
    }
    bc.defines.clear();
}

std::optional<std::string>
unfolded_defines_error(const mcpp::manifest::Manifest& m) {
    auto const& d = m.buildConfig.defines;
    if (d.empty()) return std::nullopt;
    return std::format(
        "internal error: [build].defines of package '{}' reached the build "
        "graph unfolded ({} entr{}, first '{}'); a merge ran after "
        "fold_build_defines_into_flags (please report)",
        m.package.name.empty() ? std::string("(root)") : m.package.name,
        d.size(), d.size() == 1 ? "y" : "ies", d.front());
}

// WHAT A MEMBER RECEIVES FROM ITS WORKSPACE WHEN IT IS REACHED AS A DEPENDENCY.
//
// Three parts of the inheritance matter to a dependency: `[workspace.package]`
// (a member may omit `version`), `x.workspace = true` dependency entries
// (without the merge the entry reaches resolution with neither version nor
// path), and `[workspace.build]`. They are applied at the dependency's LOAD
// site, before the conditional merge and the `defines` fold, which is the
// order the root follows; `makePackageRoot` only captures the result (#690).
// `[toolchain]`, `[target.<triple>]` and `[indices]` are decided by the root
// for the whole graph and are not applied to a dependency.
//
// One function for every way a member is reached: a sibling `path`
// dependency, a member of a git-hosted workspace, and a member inside an
// index package's archive. The same commit then compiles the same way in its
// own checkout and in every consumer's graph.
std::optional<std::string>
inherit_as_workspace_member(mcpp::manifest::Manifest& member,
                            const mcpp::manifest::Manifest& workspace,
                            const std::filesystem::path& workspaceRoot,
                            const std::filesystem::path& memberDir) {
    mcpp::project::inherit_workspace_package(member, workspace);
    mcpp::project::merge_workspace_deps(member, workspace, workspaceRoot);
    mcpp::project::inherit_workspace_build(member, workspace, workspaceRoot);
    mcpp::project::inherit_workspace_xlings(member, workspace);
    return mcpp::project::workspace_inheritance_error(member, memberDir);
}

// The workspace whose `members` list `memberDir`, searched upward from its
// parent and never above `bound` (an index package's install root: the
// archive is the only tree the package's author wrote).
std::optional<std::pair<mcpp::manifest::Manifest, std::filesystem::path>>
workspace_listing(const std::filesystem::path& memberDir,
                  const std::filesystem::path& bound) {
    auto inside = [&](const std::filesystem::path& p) {
        auto rel = p.lexically_normal().lexically_relative(bound.lexically_normal());
        return !rel.empty() && *rel.begin() != "..";
    };
    for (auto p = memberDir.parent_path(); inside(p); p = p.parent_path()) {
        if (std::filesystem::exists(p / "mcpp.toml")) {
            if (auto ws = mcpp::manifest::load(p / "mcpp.toml");
                ws && ws->workspace.present
                && mcpp::project::is_workspace_member(*ws, p, memberDir))
                return std::pair{std::move(*ws), p};
        }
        if (p == p.parent_path()) break;
    }
    return std::nullopt;
}

// ── The SECOND conditional pass: predicates that name a target-side layer ────
//
// #540/#494. `docs/14` documents a package adapting to the C library it was
// built over — `[target.'cfg(c-abi = "musl")'.build] std-module-flags =
// ["-D_GNU_SOURCE"]`, wrong for picolibc — and `stdModuleFlags` was moved onto
// BuildInputs FOR this, its member comment saying membership "is what makes the
// cfg axis carry it". Nothing evaluated the predicate: `cfgpred::Ctx` was built
// from the triple alone, so every such section was dropped in silence and the
// package built with the wrong C-library configuration, successfully.
//
// WHY A SECOND PASS AND NOT AN EARLIER CONTEXT. A layer is answerable only
// after dependency resolution — a package in the graph may supply the C library
// (openkal-musl under a `-gnu` triple), which is exactly why the triple's `env`
// segment is a REQUEST and not the answer (docs/specs/target-side.md §3.4). The
// first merge runs before resolution because conditional DEPENDENCIES have to.
//
// WHERE IT RUNS. Between `tsd::resolve` and the P1689 scan — the same window in
// which build.mcpp already contributes build inputs by mirroring into
// `packages[0]`. Everything downstream reads the snapshot from there on: the
// scan, `stdModuleFlags` collection, the fingerprint, and `compute_flags`.
//
// SCOPE. Build INPUTS only, which is what docs/14 promises ("available in
// [build] sections only"). Dependencies are excluded by construction — they are
// already resolved by now — and a section that tries is reported rather than
// silently ignored; see `warn_layer_predicate_dependencies`.
bool merge_layer_conditional_config(mcpp::manifest::Manifest& m,
                                    const cfgpred::Ctx& ctx) {
    bool any = false;
    for (auto const& cc : m.conditionalConfigs) {
        if (!cfgpred::uses_layer(cc.predicate)) continue;
        if (!cfgpred::matches(cc.predicate, ctx)) continue;
        any = true;
        mcpp::manifest::append(m.buildConfig, cc.inputs);
        // Same mirror the first pass does: `modules.sources` is the scanner's
        // own view and is not a BuildInputs member.
        for (auto const& s : cc.inputs.sources)
            m.modules.sources.push_back(s);
        for (auto const& d : cc.linkLibraryDirs)
            m.runtimeConfig.linkIntent.linkLibraryDirs.push_back(d);
        for (auto const& l : cc.libraries)
            m.runtimeConfig.linkIntent.libraries.push_back(l);
        for (auto const& f : cc.frameworks)
            m.runtimeConfig.linkIntent.frameworks.push_back(f);
        // NO `merge_conditional_xlings` HERE, DELIBERATELY. A cc that reaches
        // this pass has a layer in its predicate, and one carrying tools was
        // refused long before — see `layer_predicated_xlings_refusal`. Folding
        // it here would be folding after the tools were provisioned and after
        // every build.mcpp ran, which is the silent-absence failure the
        // refusal exists to prevent.
    }
    // Re-fold only when something was added. The call is safe either way — the
    // function clears `defines` after folding and says so — but skipping it
    // keeps this pass a no-op for the overwhelming majority of manifests, which
    // name no layer at all.
    if (any) fold_build_defines_into_flags(m.buildConfig);
    return any;
}

// Feature-activation closure — THE single implementation (build.mcpp env
// contract, Stage 2a feature-deps, and the main feature pass all call this):
// seed = [features].default ∪ requested, expanded transitively over implies;
// the literal name "default" is never itself a feature.
//
// `seedDefault` is the funnel for consumer-side `default-features = false`
// (#242, Cargo parity): when false the dependency's own `[features].default`
// is NOT seeded, so only the explicitly `requested` features (and their
// transitive `implies`) activate. The root package always seeds its own
// default (seedDefault=true); a dependency passes its dep spec's
// `defaultFeatures` flag. `requested` is applied identically either way.
std::vector<std::string> feature_closure(const mcpp::manifest::Manifest& pm,
                                         const std::vector<std::string>& requested,
                                         bool seedDefault) {
    std::vector<std::string> act, q;
    if (seedDefault)
        if (auto it = pm.featuresMap.find("default"); it != pm.featuresMap.end())
            q.insert(q.end(), it->second.begin(), it->second.end());
    q.insert(q.end(), requested.begin(), requested.end());
    std::set<std::string> seen;
    while (!q.empty()) {
        auto f = q.back(); q.pop_back();
        if (f == "default" || !seen.insert(f).second) continue;
        act.push_back(f);
        if (auto it = pm.featuresMap.find(f); it != pm.featuresMap.end())
            q.insert(q.end(), it->second.begin(), it->second.end());
    }
    return act;
}

// --features value → tokens (comma/space separated).
std::vector<std::string> feature_request_tokens(std::string_view s) {
    std::vector<std::string> out;
    for (std::size_t p = 0; p < s.size();) {
        auto c = s.find_first_of(", ", p);
        auto tok = s.substr(p, c == std::string_view::npos ? std::string_view::npos : c - p);
        if (!tok.empty()) out.emplace_back(tok);
        if (c == std::string_view::npos) break;
        p = c + 1;
    }
    return out;
}

// The root's own features among the --features tokens.
//
// A TOKEN CONTAINING `/` IS NOT A FEATURE OF THE ROOT (#649 E8). It can only
// mean "open this feature of that dependency", which is what the same token
// means inside `[features]`, so it is taken out here and applied as a forward
// of the root (`feature_forward_request`). It used to stay in this list, where
// a root without `[features]` turned it into `-DMCPP_FEATURE_SPIKE_FW_INSTALLER`
// and a root with the table reported it as an undeclared feature; neither
// opened the dependency's feature.
std::vector<std::string> parse_feature_request(std::string_view s) {
    std::vector<std::string> out;
    for (auto& tok : feature_request_tokens(s))
        if (tok.find('/') == std::string::npos) out.push_back(std::move(tok));
    return out;
}

// The `<dependency key>/<feature>` tokens of --features, in the keyspace of a
// `[features]` forward. A token with an empty half is kept whole and named by
// the caller, rather than being dropped as the manifest parser drops it: on a
// command line the user typed it just now.
std::vector<std::string> feature_forward_request_tokens(std::string_view s) {
    std::vector<std::string> out;
    for (auto& tok : feature_request_tokens(s))
        if (tok.find('/') != std::string::npos) out.push_back(std::move(tok));
    return out;
}

bool is_std_module(std::string_view name) {
    return name == "std" || name == "std.compat";
}

bool graph_or_targets_import_std(const mcpp::modgraph::Graph& graph,
                                 const mcpp::manifest::Manifest& manifest,
                                 const std::filesystem::path& projectRoot,
                                 const std::vector<mcpp::modgraph::PackageRoot>& packages) {
    return graph_or_targets_import(graph, manifest, projectRoot, packages, is_std_module);
}

// `std.compat` alone: its BMI is built when, and only when, a unit of the plan
// imports it (2026.10.5.2), so a library whose `std.compat` fails to build
// costs nothing to a project that does not use it.
bool graph_or_targets_import_std_compat(const mcpp::modgraph::Graph& graph,
                                        const mcpp::manifest::Manifest& manifest,
                                        const std::filesystem::path& projectRoot,
                                        const std::vector<mcpp::modgraph::PackageRoot>& packages) {
    return graph_or_targets_import(graph, manifest, projectRoot, packages,
                                   [](std::string_view n) { return n == "std.compat"; });
}

bool graph_or_targets_import(const mcpp::modgraph::Graph& graph,
                             const mcpp::manifest::Manifest& manifest,
                             const std::filesystem::path& projectRoot,
                             const std::vector<mcpp::modgraph::PackageRoot>& packages,
                             const std::function<bool(std::string_view)>& wanted) {
    const auto is_std_module = [&](std::string_view n) { return wanted(n); };
    for (auto& u : graph.units) {
        for (auto& req : u.requires_) {
            if (is_std_module(req.logicalName))
                return true;
        }
    }

    // Some target entry files can be added to the plan after the package scan.
    // Check them here so std BMI setup matches what make_plan will compile: they
    // are read by the same scan_entry_file make_plan reads them with. The
    // packages whose targets make_plan compiles are the root and, in a
    // workspace plan, every selected member (a member whose only sources are
    // its tests is the case the root alone misses).
    auto targets_import_std = [&](const mcpp::manifest::Manifest& m,
                                  const std::filesystem::path& root) {
        const auto extTable = mcpp::extension_table_for(m.buildConfig.moduleExtensions,
                                                        m.buildConfig.deviceExtensions);
        for (auto& t : m.targets) {
            if (t.main.empty()) continue;
            const auto entry = mcpp::modgraph::scan_entry_file(root / t.main,
                                                               m.package.name, extTable);
            for (auto const& req : entry.requires_)
                if (is_std_module(req.logicalName)) return true;
        }
        return false;
    };
    if (targets_import_std(manifest, projectRoot)) return true;
    for (auto const& pkg : packages)
        if (pkg.selectedMember && targets_import_std(pkg.manifest, pkg.root)) return true;
    return false;
}

static std::string normalizeDepLdflag(const std::filesystem::path& depRoot,
                                      const std::string& flag) {
        auto absolute_path = [&](std::string_view raw) {
            std::filesystem::path p{std::string(raw)};
            // A loader token stays as written; see the predicate.
            if (p.is_absolute() || mcpp::build::is_loader_relative_search_path(raw))
                return p;
            return depRoot / p;
        };

        if (flag.starts_with("-L") && flag.size() > 2) {
            return "-L" + absolute_path(std::string_view(flag).substr(2)).string();
        }

        constexpr std::string_view rpathPrefix = "-Wl,-rpath,";
        if (flag.starts_with(rpathPrefix) && flag.size() > rpathPrefix.size()) {
            return std::string(rpathPrefix)
                 + absolute_path(std::string_view(flag).substr(rpathPrefix.size())).string();
        }

        return flag;
}

// A dependency's link flags as its consumer's link reads them. Word by word
// (SPEC-004 §8, #703): a search path is made absolute per word, and each word
// is written back as an element that reads as exactly that word, so the
// consumer's renderer reads the dependency's flags with the same reading its
// own flags receive, and an element that packs several tokens is several
// words on both sides.
std::vector<std::string> normalized_dependency_ldflags(
        const std::filesystem::path& depRoot, const std::vector<std::string>& ldflags) {
        std::vector<std::string> out;
        for (auto const& word : mcpp::manifest::flag_words(ldflags))
            out.push_back(mcpp::manifest::flag_element(normalizeDepLdflag(depRoot, word)));
        return out;
}

} // namespace mcpp::build
