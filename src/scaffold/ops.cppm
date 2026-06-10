// mcpp.scaffold.ops — project creation: package-shipped templates
// (fetch + instantiate) and the builtin bin/gui skeletons.
// Bodies moved verbatim from the CLI layer. Zero behavior change.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.scaffold.ops;

import std;
import mcpp.config;
import mcpp.fetcher;
import mcpp.fetcher.progress;
import mcpp.manifest;
import mcpp.pm.resolver;
import mcpp.scaffold;
import mcpp.ui;

namespace mcpp::scaffold {

// ─── Package-based templates (design v2: multi-level --template) ──────
//
// Resolve SPEC's package@version through the index, ensure the package
// sources are installed (same cache as dependencies), and return the
// package root (the directory containing mcpp.toml).
struct FetchedTemplatePackage {
    std::filesystem::path root;
    std::string           name;     // short package name (e.g. "imgui")
    std::string           version;  // resolved exact version
};

std::expected<FetchedTemplatePackage, std::string>
fetch_template_package(const mcpp::scaffold::TemplateSpec& spec) {
    auto cfg = mcpp::config::load_or_init(/*quiet=*/false,
        mcpp::fetcher::make_bootstrap_progress_callback());
    if (!cfg) return std::unexpected(cfg.error().message);
    mcpp::pm::Fetcher fetcher(*cfg);

    // Namespace candidates mirror dependency lookup: index root first,
    // then the compat namespace.
    std::string ns;
    std::optional<std::string> lua;
    for (std::string cand : {std::string{}, std::string{"compat"}}) {
        if (auto l = fetcher.read_xpkg_lua(cand, spec.pkg)) {
            ns = cand;
            lua = std::move(*l);
            break;
        }
    }
    if (!lua) {
        return std::unexpected(std::format(
            "template package '{}' not found in the index "
            "(check the name, or run `mcpp index update`)", spec.pkg));
    }

    std::string version = spec.version;
    if (version.empty()) {
        auto v = mcpp::pm::resolve_semver(ns, spec.pkg, "*", fetcher);
        if (!v) return std::unexpected(v.error());
        version = *v;
    }

    auto installed = fetcher.install_path(ns, spec.pkg, version);
    if (!installed) {
        auto fq = ns.empty() ? spec.pkg : std::format("{}.{}", ns, spec.pkg);
        mcpp::ui::info("Downloading", std::format("{} v{}", fq, version));
        mcpp::fetcher::InstallProgressHandler progress;
        std::vector<std::string> targets{ std::format("{}@{}", fq, version) };
        auto r = fetcher.install(targets, &progress);
        if (!r) return std::unexpected(std::format(
            "fetch '{}@{}': {}", fq, version, r.error().message));
        if (r->exitCode != 0) return std::unexpected(std::format(
            "fetch '{}@{}' failed (exit {})", fq, version, r->exitCode));
        installed = fetcher.install_path(ns, spec.pkg, version);
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
            "package '{}@{}' has no mcpp.toml", spec.pkg, version));
    }
    return FetchedTemplatePackage{root, spec.pkg, version};
}

void print_template_listing(const FetchedTemplatePackage& pkg,
                            const std::vector<mcpp::scaffold::TemplateEntry>& entries) {
    std::println("Templates in {}@{}:", pkg.name, pkg.version);
    for (auto& t : entries) {
        std::println("  {:<14}{}{}", t.name,
                     t.meta.isDefault ? "(default)  " : "           ",
                     t.meta.description);
    }
    std::println("");
    std::println("usage: mcpp new <name> --template {}[@ver][:<template>]", pkg.name);
}

export int list_package_templates(const mcpp::scaffold::TemplateSpec& spec) {
    auto pkg = fetch_template_package(spec);
    if (!pkg) { mcpp::ui::error(pkg.error()); return 1; }
    auto entries = mcpp::scaffold::list_templates(pkg->root);
    if (!entries) { mcpp::ui::error(entries.error()); return 1; }
    print_template_listing(*pkg, *entries);
    return 0;
}

export int new_from_package_template(const std::string& name, const std::string& specStr) {
    auto spec = mcpp::scaffold::parse_spec(specStr);
    if (spec.listOnly) return list_package_templates(spec);

    auto pkg = fetch_template_package(spec);
    if (!pkg) { mcpp::ui::error(pkg.error()); return 1; }
    auto entries = mcpp::scaffold::list_templates(pkg->root);
    if (!entries) { mcpp::ui::error(entries.error()); return 1; }

    const mcpp::scaffold::TemplateEntry* chosen = nullptr;
    if (spec.tmpl.empty()) {
        for (auto& t : *entries) if (t.meta.isDefault) { chosen = &t; break; }
        if (!chosen) {
            mcpp::ui::error(std::format(
                "package '{}' declares no default template — pick one explicitly:",
                pkg->name));
            print_template_listing(*pkg, *entries);
            return 1;
        }
    } else {
        for (auto& t : *entries) if (t.name == spec.tmpl) { chosen = &t; break; }
        if (!chosen) {
            mcpp::ui::error(std::format(
                "package '{}@{}' has no template '{}'",
                pkg->name, pkg->version, spec.tmpl));
            print_template_listing(*pkg, *entries);
            return 1;
        }
    }

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

    mcpp::scaffold::RenderVars vars{name, pkg->name, pkg->version};
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
        "{} (template {}@{}:{})", name, pkg->name, pkg->version, chosen->name));
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
        os << "target/\n";
    }

    std::println("Created {} package '{}' at {}", gui ? "gui" : "bin", name, root.string());
    std::println("Next: cd {} && mcpp build && mcpp run  (or `mcpp test`)", name);
    return 0;
}

} // namespace mcpp::scaffold
