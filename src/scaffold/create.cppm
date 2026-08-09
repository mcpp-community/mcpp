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
    const std::string& name,
    const mcpp::scaffold::TemplateSpec& spec) {
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

    std::filesystem::path root = std::filesystem::current_path() / name;
    if (std::filesystem::exists(root)) {
        mcpp::ui::error(std::format("'{}' already exists", root.string()));
        return 1;
    }
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec) {
        mcpp::ui::error(std::format("cannot create '{}': {}",
                                    root.string(), ec.message()));
        return 1;
    }

    // Keep the complete resolved PackageId through rendering/injection. The
    // current renderer's `self.name` surface is the canonical selector; Task 4
    // expands this into separately addressable namespace/name variables.
    mcpp::scaffold::RenderVars vars{name, pkg->selector, pkg->version};
    if (auto err = mcpp::scaffold::instantiate(
            pkg->root / "templates" / chosen->name, root, vars)) {
        mcpp::ui::error(*err);
        return 1;
    }
    if (auto err = mcpp::scaffold::inject_self_dependency(
            root / "mcpp.toml", vars, chosen->meta.injectSelfFeatures)) {
        mcpp::ui::error(*err);
        return 1;
    }

    mcpp::ui::status("Created", std::format(
        "{} (template {}@{}:{})", name, pkg->selector, pkg->version,
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
export int create_builtin_project(const std::string& name, bool gui) {
    std::filesystem::path root = std::filesystem::current_path() / name;
    if (std::filesystem::exists(root)) {
        std::println(stderr, "error: '{}' already exists", root.string());
        return 1;
    }
    std::error_code ec;
    std::filesystem::create_directories(root / "src", ec);
    if (ec) {
        std::println(stderr, "error: cannot create '{}': {}", root.string(), ec.message());
        return 1;
    }

    // mcpp.toml
    {
        std::ofstream os(root / "mcpp.toml");
        os << mcpp::manifest::default_template(name);
        if (gui) {
            // The GUI template depends on the imgui module package. It does not
            // pin a toolchain — mcpp resolves the environment/default toolchain
            // and the GL runtime is closed by the ecosystem (compat.glx-runtime).
            os << "\n[dependencies]\nimgui = \"0.0.5\"\n";
        }
    }
    // src/main.cpp — template with PROJECT placeholder, replaced with `name`.
    {
        std::string body = gui ? R"GUI(// PROJECT — generated by `mcpp new --template gui`
// Tier-0 zero-boilerplate window via the imgui.app facade. No #include.
import imgui.core;
import imgui.app;

int main() {
    return ImGui::App::run([] {
        ImGui::Begin("PROJECT");
        ImGui::TextUnformatted("Hello from mcpp + imgui (imgui.app facade)");
        ImGui::End();
    });
}
)GUI" : R"(// PROJECT — generated by `mcpp new`
import std;

int main(int argc, char* argv[]) {
    std::println("Hello from PROJECT!");
    std::println("Built with import std + std::println on modular C++23.");
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) std::println("  arg[{}] = {}", i, argv[i]);
    }
    return 0;
}
)";
        std::size_t pos;
        while ((pos = body.find("PROJECT")) != std::string::npos) {
            body.replace(pos, 7, name);
        }
        std::ofstream os(root / "src" / "main.cpp");
        os << body;
    }
    // tests/test_smoke.cpp — bundled smoke test (`mcpp test` works out-of-the-box).
    {
        std::filesystem::create_directories(root / "tests", ec);
        std::ofstream os(root / "tests" / "test_smoke.cpp");
        os << R"(// Smoke test — verifies the project compiles + a binary runs.
// Add more tests as tests/test_*.cpp files; mcpp test discovers them
// automatically (one binary per file).
import std;

int main() {
    std::println("test_smoke: ok");
    return 0;
}
)";
    }
    // .gitignore
    {
        std::ofstream os(root / ".gitignore");
        // `.mcpp/` is the per-project xlings sandbox (and, when no MCPP_HOME
        // can be resolved, the local BMI cache) — build state, never sources.
        os << "target/\n.mcpp/\n";
    }

    std::println("Created {} package '{}' at {}", gui ? "gui" : "bin", name, root.string());
    std::println("Next: cd {} && mcpp build && mcpp run  (or `mcpp test`)", name);
    return 0;
}

} // namespace mcpp::scaffold
