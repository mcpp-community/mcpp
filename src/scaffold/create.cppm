// mcpp.scaffold.create — project creation: exact package-shipped templates
// (resolve + fetch + instantiate) and the builtin bin/gui skeletons.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.scaffold.create;

import std;
import mcpp.config;
import mcpp.fetcher;
import mcpp.fetcher.progress;
import mcpp.manifest;
import mcpp.pm.dep_spec;
import mcpp.pm.dependency_selector;
import mcpp.pm.index_route;
import mcpp.platform.axis;
import mcpp.pm.resolver;
import mcpp.scaffold;
import mcpp.scaffold.project_name;
import mcpp.ui;

namespace mcpp::scaffold {

// ─── Package-based templates (exact [ns.]name[@ver][:tname]) ─────────
//
// Resolve SPEC's package@version through the index, ensure the package
// sources are installed (same cache as dependencies), and return the
// package root (the directory containing mcpp.toml).
struct FetchedTemplatePackage {
    std::filesystem::path          root;
    mcpp::pm::DependencyCoordinate id;
    std::string                    selector;
    std::string                    version;
    std::string                    indexRoute;
    std::string                    descriptorDigest;
    std::string                    payloadDigest;
};

std::string stable_text_digest(std::string_view text) {
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char ch : text) {
        hash ^= ch;
        hash *= 1099511628211ull;
    }
    return std::format("fnv1a:{:016x}", hash);
}

std::expected<FetchedTemplatePackage, std::string>
fetch_template_package(const mcpp::scaffold::TemplateSpec& spec) {
    auto cfg = mcpp::config::load_or_init(/*quiet=*/false,
        mcpp::fetcher::make_bootstrap_progress_callback());
    if (!cfg) return std::unexpected(cfg.error().message);
    mcpp::pm::Fetcher fetcher(*cfg);

    const auto coordinate = mcpp::pm::normalize_package_selector(spec.package);
    const auto selector = mcpp::pm::format_package_selector(coordinate);

    // `mcpp new` has no target project manifest yet, so template packages come
    // from the configured registry/mcpp-index. The lookup itself is still the
    // same exact IndexRoute identity gate used by dependencies.
    mcpp::pm::IndexRoute registryOnly{ nullptr, {}, &*cfg };
    auto direct = mcpp::pm::make_direct_dependency_selector(
        coordinate.namespace_, coordinate.shortName, selector);
    auto found = mcpp::pm::lookup_descriptor(
        registryOnly, direct.candidates);
    if (!found.hit) {
        auto defaultNote = spec.package.namespace_
            ? std::string{}
            : std::format(" (namespace omitted means '{}')",
                          mcpp::pm::kDefaultNamespace);
        auto namespaceHint = (!spec.package.namespace_ && spec.templateName)
            ? std::format(
                " ':' selects a template inside package '{}'; if you intended "
                "a namespace, write '{}.{}'.",
                spec.package.name, spec.package.name, *spec.templateName)
            : std::string{};
        return std::unexpected(std::format(
            "template package '{}'{} not found for exact identity ({}, {}) "
            "in mcpp-index (check the selector, or run `mcpp index update`).{}",
            selector, defaultNote, coordinate.namespace_, coordinate.shortName,
            namespaceHint));
    }
    auto lua = std::move(found.hit->lua);

    // Form B packages are inline build recipes with no source tree, hence no
    // physical templates directory. Report the provider error rather than
    // pretending that a different namespace might satisfy the selector.
    auto field = mcpp::manifest::extract_mcpp_field(lua);
    if (field.kind == mcpp::manifest::McppField::TableBody) {
        return std::unexpected(std::format(
            "template package '{}' is an inline build recipe and ships no "
            "source template payload", selector));
    }

    const std::string& ns = coordinate.namespace_;
    const std::string& shortName = coordinate.shortName;
    const auto constraint = spec.version
        ? std::format("={}", *spec.version)
        : std::string{"*"};
    auto resolved = mcpp::pm::resolve_semver(
        ns, shortName, constraint, registryOnly,
        mcpp::platform::HostPlatform::current());
    if (!resolved) return std::unexpected(resolved.error());
    std::string version = std::move(*resolved);

    std::string payloadDigest;
    bool resolvedAlias = false;
    for (auto const& entry : mcpp::manifest::list_xpkg_version_entries(
             lua, mcpp::platform::HostPlatform::current())) {
        if (entry.version == version) {
            payloadDigest = entry.sha256;
            resolvedAlias = entry.alias;
            break;
        }
    }
    if (resolvedAlias) {
        return std::unexpected(std::format(
            "template package '{}@{}' names a moving/indirect version alias; "
            "pin the concrete version entry instead", selector, version));
    }

    auto installed = fetcher.install_path(ns, shortName, version);
    if (!installed) {
        // Human-facing name is the resolved identity; the WIRE address is the
        // descriptor's own `<namespace>:<literal name>` (SPEC-001 §6). Deriving
        // it as `<ns>.<short>` only matched while every descriptor spelled
        // `name` fully-qualified — the short-name migration killed that, the
        // same way it killed the dependency path.
        auto fq = ns.empty() ? shortName : std::format("{}.{}", ns, shortName);
        auto wireAddr = mcpp::manifest::xpkg_wire_address(lua, ns, shortName);
        mcpp::ui::info("Downloading", std::format("{} v{}", fq, version));
        mcpp::fetcher::InstallProgressHandler progress;
        std::vector<std::string> targets{
            std::format("{}@{}", wireAddr.target, version) };
        auto r = fetcher.install(targets, &progress);
        if (!r) return std::unexpected(std::format(
            "fetch '{}@{}': {}", fq, version, r.error().message));
        if (r->exitCode != 0) return std::unexpected(std::format(
            "fetch '{}@{}' failed (exit {})", fq, version, r->exitCode));
        installed = fetcher.install_path(ns, shortName, version);
        if (!installed) return std::unexpected(std::format(
            "package '{}@{}' install path missing after fetch", fq, version));
    }

    // Package root = the directory holding mcpp.toml (tarballs usually wrap
    // everything in a single top-level directory).
    std::filesystem::path root = *installed;
    if (!std::filesystem::exists(root / "mcpp.toml")) {
        std::error_code ec;
        for (auto& e : std::filesystem::directory_iterator(root, ec)) {
            if (e.is_directory()
                && std::filesystem::exists(e.path() / "mcpp.toml")) {
                root = e.path();
                break;
            }
        }
    }
    if (!std::filesystem::exists(root / "mcpp.toml")) {
        return std::unexpected(std::format(
            "package '{}@{}' has no mcpp.toml", selector, version));
    }
    return FetchedTemplatePackage{
        .root = std::move(root),
        .id = coordinate,
        .selector = selector,
        .version = version,
        .indexRoute = std::format("mcpp-index:{}", ns),
        .descriptorDigest = stable_text_digest(lua),
        .payloadDigest = std::move(payloadDigest),
    };
}

void print_template_listing(const FetchedTemplatePackage& pkg,
                            const std::vector<mcpp::scaffold::TemplateEntry>& entries) {
    std::println("Templates in {}@{}:", pkg.selector, pkg.version);
    for (auto& t : entries) {
        std::println("  {:<14}{}{}", t.name,
                     t.meta.isDefault ? "(default)  " : "           ",
                     t.meta.description);
    }
    std::println("");
    std::println("usage: mcpp new <name> --template {}[@ver][:<template>]",
                 pkg.selector);
}

export int list_package_templates(const mcpp::scaffold::TemplateSpec& spec) {
    auto pkg = fetch_template_package(spec);
    if (!pkg) { mcpp::ui::error(pkg.error()); return 1; }
    auto entries = mcpp::scaffold::list_templates(pkg->root);
    if (!entries) { mcpp::ui::error(entries.error()); return 1; }
    print_template_listing(*pkg, *entries);
    return 0;
}

export int new_from_package_template(
    const mcpp::scaffold::PortableProjectName& project,
    const mcpp::scaffold::TemplateSpec& spec) {
    const auto parent = std::filesystem::current_path();
    const auto finalPath = parent / project.directoryName;
    std::error_code existenceError;
    if (std::filesystem::exists(finalPath, existenceError)
        || existenceError) {
        mcpp::ui::error(existenceError
            ? std::format("cannot inspect '{}': {}", finalPath.string(),
                          existenceError.message())
            : std::format("'{}' already exists", finalPath.string()));
        return 1;
    }

    auto pkg = fetch_template_package(spec);
    if (!pkg) { mcpp::ui::error(pkg.error()); return 1; }
    auto entries = mcpp::scaffold::list_templates(pkg->root);
    if (!entries) { mcpp::ui::error(entries.error()); return 1; }

    auto chosenResult = mcpp::scaffold::select_template(
        *entries,
        spec.templateName
            ? std::optional<std::string_view>{*spec.templateName}
            : std::nullopt);
    if (!chosenResult) {
        mcpp::ui::error(std::format(
            "template package '{}@{}': {}",
            pkg->selector, pkg->version, chosenResult.error()));
        print_template_listing(*pkg, *entries);
        return 1;
    }
    const auto* chosen = *chosenResult;

    auto transaction = mcpp::scaffold::ScaffoldTransaction::begin(
        parent, project.directoryName);
    if (!transaction) {
        mcpp::ui::error(transaction.error());
        return 1;
    }
    auto tx = std::move(*transaction);
    const auto& root = tx.staging_path();

    // Keep both identities fully typed through rendering and dependency
    // injection; a same-short-name package in another namespace is distinct.
    mcpp::scaffold::RenderVars vars{
        .projectName = project.name,
        .projectNamespace = project.namespace_,
        .projectQualifiedName = project.qualifiedName,
        .templatePackageNamespace = pkg->id.namespace_,
        .templatePackageName = pkg->id.shortName,
        .templatePackageSelector = pkg->selector,
        .templatePackageVersion = pkg->version,
        .templateName = chosen->name,
    };
    auto instantiated = mcpp::scaffold::instantiate(
        pkg->root / "templates" / chosen->name, root, vars);
    if (!instantiated) {
        mcpp::ui::error(instantiated.error());
        return 1;
    }
    auto injected = mcpp::scaffold::inject_self_dependency(
        root / "mcpp.toml", vars, chosen->meta.injectSelfFeatures);
    if (!injected) {
        mcpp::ui::error(injected.error());
        return 1;
    }

    // Parsing the completed manifest is the final semantic gate. Nothing is
    // user-visible until the same-filesystem rename below succeeds.
    auto generated = mcpp::manifest::load(root / "mcpp.toml");
    if (!generated) {
        mcpp::ui::error(std::format(
            "generated manifest validation failed: {}",
            generated.error().format()));
        return 1;
    }
    if (auto committed = tx.commit(); !committed) {
        mcpp::ui::error(committed.error());
        return 1;
    }

    mcpp::ui::status("Created", std::format(
        "{} (template {}@{}:{})", project.qualifiedName,
        pkg->selector, pkg->version,
        chosen->name));
    std::println("Resolved template package: namespace={} name={} route={} "
                 "descriptor={} payload={}",
                 pkg->id.namespace_, pkg->id.shortName, pkg->indexRoute,
                 pkg->descriptorDigest,
                 pkg->payloadDigest.empty() ? "unavailable" : pkg->payloadDigest);
    if (!chosen->meta.postMessage.empty())
        std::println("{}", chosen->meta.postMessage);
    return 0;
}

// Builtin `mcpp new` skeleton (bin, plus the transitional gui alias).
export int create_builtin_project(
    const mcpp::scaffold::PortableProjectName& project, bool gui) {
    auto transaction = mcpp::scaffold::ScaffoldTransaction::begin(
        std::filesystem::current_path(), project.directoryName);
    if (!transaction) {
        mcpp::ui::error(transaction.error());
        return 1;
    }
    auto tx = std::move(*transaction);
    const auto& root = tx.staging_path();

    std::error_code ec;
    std::filesystem::create_directories(root / "src", ec);
    if (ec) {
        mcpp::ui::error(std::format(
            "cannot create scaffold source directory: {}", ec.message()));
        return 1;
    }

    // mcpp.toml
    {
        std::string manifest = mcpp::manifest::default_template(
            project.qualifiedName);
        if (gui) {
            // The GUI template depends on the imgui module package. It does not
            // pin a toolchain — mcpp resolves the environment/default toolchain
            // and the GL runtime is closed by the ecosystem (compat.glx-runtime).
            manifest += "\n[dependencies]\nimgui = \"0.0.5\"\n";
        }
        if (auto written = mcpp::scaffold::write_text_file(
                root / "mcpp.toml", manifest); !written) {
            mcpp::ui::error(written.error());
            return 1;
        }
    }
    // src/main.cpp — rendered once; values are never rescanned as template
    // source.
    {
        std::string_view source = gui ? R"GUI(// {{project.name}} — generated by `mcpp new --template gui`
// Tier-0 zero-boilerplate window via the imgui.app facade. No #include.
import imgui.core;
import imgui.app;

int main() {
    return ImGui::App::run([] {
        ImGui::Begin("{{project.name}}");
        ImGui::TextUnformatted("Hello from mcpp + imgui (imgui.app facade)");
        ImGui::End();
    });
}
)GUI" : R"(// {{project.name}} — generated by `mcpp new`
import std;

int main(int argc, char* argv[]) {
    std::println("Hello from {{project.name}}!");
    std::println("Built with import std + std::println on modular C++23.");
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) std::println("  arg[{}] = {}", i, argv[i]);
    }
    return 0;
}
)";
        mcpp::scaffold::RenderVars vars{
            .projectName = project.name,
            .projectNamespace = project.namespace_,
            .projectQualifiedName = project.qualifiedName,
            .templateName = gui ? "gui" : "bin",
        };
        auto body = mcpp::scaffold::render_tokens(source, vars);
        if (!body) {
            mcpp::ui::error(body.error().message);
            return 1;
        }
        if (auto written = mcpp::scaffold::write_text_file(
                root / "src" / "main.cpp", *body); !written) {
            mcpp::ui::error(written.error());
            return 1;
        }
    }
    // tests/test_smoke.cpp — bundled smoke test (`mcpp test` works out-of-the-box).
    {
        std::filesystem::create_directories(root / "tests", ec);
        if (ec) {
            mcpp::ui::error(std::format(
                "cannot create scaffold test directory: {}", ec.message()));
            return 1;
        }
        constexpr std::string_view smoke = R"(// Smoke test — verifies the project compiles + a binary runs.
// Add more tests as tests/test_*.cpp files; mcpp test discovers them
// automatically (one binary per file).
import std;

int main() {
    std::println("test_smoke: ok");
    return 0;
}
)";
        if (auto written = mcpp::scaffold::write_text_file(
                root / "tests" / "test_smoke.cpp", smoke); !written) {
            mcpp::ui::error(written.error());
            return 1;
        }
    }
    // .gitignore
    {
        // `.mcpp/` is the per-project xlings sandbox (and, when no MCPP_HOME
        // can be resolved, the local BMI cache) — build state, never sources.
        if (auto written = mcpp::scaffold::write_text_file(
                root / ".gitignore", "target/\n.mcpp/\n"); !written) {
            mcpp::ui::error(written.error());
            return 1;
        }
    }

    auto generated = mcpp::manifest::load(root / "mcpp.toml");
    if (!generated) {
        mcpp::ui::error(std::format(
            "generated manifest validation failed: {}",
            generated.error().format()));
        return 1;
    }
    if (auto committed = tx.commit(); !committed) {
        mcpp::ui::error(committed.error());
        return 1;
    }

    std::println("Created {} package '{}' at {}", gui ? "gui" : "bin",
                 project.qualifiedName, tx.final_path().string());
    std::println("Next: cd {} && mcpp build && mcpp run  (or `mcpp test`)",
                 project.directoryName);
    return 0;
}

} // namespace mcpp::scaffold
