// mcpp.scaffold.create — project creation: exact package-shipped templates
// (resolve + fetch + instantiate) and the builtin bin/gui skeletons.

export module mcpp.scaffold.create;

import std;
import mcpp.config;
import mcpp.fetcher;
import mcpp.fetcher.progress;
import mcpp.manifest;
import mcpp.pm.dep_spec;
import mcpp.pm.dependency_selector;
import mcpp.pm.index_route;
import mcpp.toolchain.platform;
import mcpp.pm.resolver;
import mcpp.scaffold;
import mcpp.scaffold.project_name;
import mcpp.ui;
import mcpp.build.test_protocol;   // the release of mtest the skeleton's tests use
import mcpp.toolchain.target_schema;
import mcpp.toolchain.target_rows;   // the llvm a platform skeleton pins, as the built-in rows do

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
    if (!found.error.empty()) return std::unexpected(found.error);
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
        mcpp::toolchain::platform::HostPlatform::current());
    if (!resolved) return std::unexpected(resolved.error());
    std::string version = std::move(*resolved);

    std::string payloadDigest;
    bool resolvedAlias = false;
    for (auto const& entry : mcpp::manifest::list_xpkg_version_entries(
             lua, mcpp::toolchain::platform::HostPlatform::current())) {
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
    // A template may decline the self-dependency — see TemplateMeta::injectSelf
    // for the case that requires it.
    if (chosen->meta.injectSelf) {
        auto injected = mcpp::scaffold::inject_self_dependency(
            root / "mcpp.toml", vars, chosen->meta.injectSelfFeatures);
        if (!injected) {
            mcpp::ui::error(injected.error());
            return 1;
        }
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
    // What `new` says about what it made is narration, like the `Created`
    // line above it: standard error, so that the project's creation is not
    // part of what a pipe receives.
    mcpp::ui::line(std::format(
        "Resolved template package: namespace={} name={} route={} "
        "descriptor={} payload={}",
        pkg->id.namespace_, pkg->id.shortName, pkg->indexRoute,
        pkg->descriptorDigest,
        pkg->payloadDigest.empty() ? "unavailable" : pkg->payloadDigest));
    if (!chosen->meta.postMessage.empty())
        mcpp::ui::line(chosen->meta.postMessage);
    return 0;
}

// The builtin skeletons of `mcpp new`: a program (bin, and the transitional
// gui alias) and a library (lib).
export enum class builtin { bin, gui, lib };

// `mcpp new --template platform` (SPEC-016 §2, §3): a platform package that
// defines one row, `<arch>-<name>`, and the provider that serves it, with the
// llvm the built-in bare-metal rows pin. `mcpp check platform` reads it.
export int create_platform_project(const mcpp::scaffold::PortableProjectName& project);

// What a project's library is named in C++: its module, the qualified name
// with `-` written `_` (`acme.my-lib` -> `acme.my_lib`), and its namespace
// (`acme::my_lib`). Empty when a component does not begin with a letter or
// `_`, which no module name may.
struct cxx_names {
    std::string module;
    std::string space;
};
std::optional<cxx_names> cxx_names_of(std::string_view qualified) {
    cxx_names out;
    for (auto part : std::views::split(qualified, '.')) {
        std::string component(part.begin(), part.end());
        std::ranges::replace(component, '-', '_');
        if (component.empty() || !(std::isalpha(static_cast<unsigned char>(component.front()))
                                   || component.front() == '_'))
            return std::nullopt;
        out.module += (out.module.empty() ? "" : ".") + component;
        out.space += (out.space.empty() ? "" : "::") + component;
    }
    return out;
}

// The test framework a skeleton's tests use (docs/08): mtest, whose entry
// begins a test file that defines no `main`.
std::string test_dependency() {
    return std::format("\n[dev-dependencies]\nmcpplibs.mtest = \"{}\"\n",
                       mcpp::build::test_protocol::kMtestRelease);
}

export int create_builtin_project(
    const mcpp::scaffold::PortableProjectName& project, builtin kind) {
    const bool gui = kind == builtin::gui;
    const bool lib = kind == builtin::lib;
    std::optional<cxx_names> names;
    if (lib) {
        names = cxx_names_of(project.qualifiedName);
        if (!names) {
            mcpp::ui::error(std::format(
                "a library's module is named after the package, and '{}' has a part that does not "
                "begin with a letter or '_'", project.qualifiedName));
            return 2;
        }
    }
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
        manifest += test_dependency();
        if (auto written = mcpp::scaffold::write_text_file(
                root / "mcpp.toml", manifest); !written) {
            mcpp::ui::error(written.error());
            return 1;
        }
    }
    // src/main.cpp — rendered once; values are never rescanned as template
    // source.
    if (!lib) {
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
            .projectModule = {},
            .projectCxxNamespace = {},
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
    // A library: its module, a partition it exports and the partition's
    // implementation, each rendered once.
    if (lib) {
        constexpr std::array<std::pair<std::string_view, std::string_view>, 3> files {{
            {"{{project.name}}.cppm", R"(// The module {{project.module}}: what a consumer imports (`import {{project.module}};`).
export module {{project.module}};

export import :hello;
)"},
            {"hello.cppm", R"(// The partition {{project.module}}:hello, which the module exports.
export module {{project.module}}:hello;

import std;

export namespace {{project.cxxNamespace}} {

// A greeting for `name`.
std::string hello(std::string_view name);

}  // namespace {{project.cxxNamespace}}
)"},
            {"hello.cpp", R"(// The implementation of hello, a unit of the module {{project.module}}.
module {{project.module}};

import std;

namespace {{project.cxxNamespace}} {

std::string hello(std::string_view name) {
    return std::format("Hello, {}!", name);
}

}  // namespace {{project.cxxNamespace}}
)"},
        }};
        mcpp::scaffold::RenderVars vars{
            .projectName = project.name,
            .projectNamespace = project.namespace_,
            .projectQualifiedName = project.qualifiedName,
            .templateName = "lib",
            .projectModule = names->module,
            .projectCxxNamespace = names->space,
        };
        for (const auto& [name, text] : files) {
            auto fileName = mcpp::scaffold::render_tokens(name, vars);
            auto body = mcpp::scaffold::render_tokens(text, vars);
            if (!fileName || !body) {
                mcpp::ui::error(!fileName ? fileName.error().message : body.error().message);
                return 1;
            }
            if (auto written = mcpp::scaffold::write_text_file(root / "src" / *fileName, *body); !written) {
                mcpp::ui::error(written.error());
                return 1;
            }
        }
    }
    // tests/ — the skeleton's test, a file of mtest's cases with no `main`:
    // `mcpp test` lists, selects and reports each case (docs/08).
    {
        std::filesystem::create_directories(root / "tests", ec);
        if (ec) {
            mcpp::ui::error(std::format(
                "cannot create scaffold test directory: {}", ec.message()));
            return 1;
        }
        constexpr std::string_view smoke = R"(// A test of {{project.name}}: each object is a case `mcpp test` lists, selects and reports.
// More tests are more objects, or more files under tests/; a file defines no `main`.
import mcpplibs.mtest;

using namespace mcpplibs::mtest;

UTest smoke{"the project builds and its test runs", [] { expect(eq(1 + 1, 2)); }};
)";
        constexpr std::string_view greeting = R"(// The cases of {{project.module}}: each object is one `mcpp test` lists, selects and reports.
// More tests are more objects, or more files under tests/; a file defines no `main`.
import std;
import mcpplibs.mtest;
import {{project.module}};

using namespace mcpplibs::mtest;

UTest greeting{"hello greets by name", [] {
    expect(eq({{project.cxxNamespace}}::hello("mcpp"), std::string("Hello, mcpp!")));
}};
)";
        mcpp::scaffold::RenderVars vars{
            .projectName = project.name,
            .projectNamespace = project.namespace_,
            .projectQualifiedName = project.qualifiedName,
            .templateName = lib ? "lib" : gui ? "gui" : "bin",
            .projectModule = names ? names->module : std::string{},
            .projectCxxNamespace = names ? names->space : std::string{},
        };
        auto body = mcpp::scaffold::render_tokens(lib ? greeting : smoke, vars);
        if (!body) {
            mcpp::ui::error(body.error().message);
            return 1;
        }
        if (auto written = mcpp::scaffold::write_text_file(
                root / "tests" / (lib ? "hello.cpp" : "test_smoke.cpp"), *body); !written) {
            mcpp::ui::error(written.error());
            return 1;
        }
    }
    // .gitignore
    {
        // `.mcpp/` is the per-project xlings sandbox (and, when no MCPP_HOME
        // can be resolved, the local BMI cache) — build state, never sources.
        // `compile_commands.json` at the root names absolute paths of one
        // machine's build (design 2026-09-26 §3.2): it is a copy of the
        // current configuration's own database under `target/`, replaced
        // whole on the next build, so a checkout gains nothing from shipping
        // it and loses a warning the first time another machine's copy
        // disagrees with this one.
        if (auto written = mcpp::scaffold::write_text_file(
                root / ".gitignore", "target/\n.mcpp/\ncompile_commands.json\n"); !written) {
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

    mcpp::ui::line(std::format("Created {} package '{}' at {}", lib ? "lib" : gui ? "gui" : "bin",
                               project.qualifiedName, tx.final_path().string()));
    mcpp::ui::line(lib ? std::format("Next: cd {} && mcpp build && mcpp test", project.directoryName)
                       : std::format("Next: cd {} && mcpp build && mcpp run  (or `mcpp test`)",
                                     project.directoryName));
    return 0;
}

int create_platform_project(const mcpp::scaffold::PortableProjectName& project) {
    // The system's word: the package's name, letters and digits.
    std::string os;
    for (char c : project.name)
        if (std::isalnum(static_cast<unsigned char>(c))) os += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (os.empty() || !std::isalpha(static_cast<unsigned char>(os.front()))) os = "example";
    std::string llvm;
    for (auto const* p : mcpp::toolchain::target::providers())
        if (p->toolchain.starts_with("llvm@")) { llvm = std::string(p->toolchain); break; }
    const std::string arch(mcpp::toolchain::platform::host_arch);
    const std::string triple = std::format("{}-{}", arch, os);

    auto transaction = mcpp::scaffold::ScaffoldTransaction::begin(
        std::filesystem::current_path(), project.directoryName);
    if (!transaction) {
        mcpp::ui::error(transaction.error());
        return 1;
    }
    auto tx = std::move(*transaction);
    const auto& root = tx.staging_path();
    const std::string manifest = std::format(R"(# A platform (SPEC-016): the rows it defines and the provider that serves them.
# A project selects it for a target with [target.<triple>] platform = {{ path = "..." }}.
[package]
name    = "{0}"
version = "0.1.0"
description = "The platform of {1}"

[platform]
toolchain = "{2}"

# The row: the system's triple, what the compiler is told, the object format,
# and the system its programs run on.
[[platform.define]]
triple        = "{1}"
clang-triple  = "{3}-unknown-none-elf"
object-format = "elf"
execution     = "{4}"

[[platform.provider]]
serves = ["{1}"]

[platform.build]
cflags = ["-ffreestanding"]

[platform.link]
driver = "compiler"

# What the platform provides for the target side; `false`: nothing yet.
[platform.layers]
"kernel-abi"       = false
"c-abi"            = false
"c++-abi"          = false
"compiler-runtime" = false
)", project.qualifiedName, triple, llvm, arch, os);
    if (auto written = mcpp::scaffold::write_text_file(root / "mcpp.toml", manifest); !written) {
        mcpp::ui::error(written.error());
        return 1;
    }
    if (auto written = mcpp::scaffold::write_text_file(root / ".gitignore", "target/\n.mcpp/\n"); !written) {
        mcpp::ui::error(written.error());
        return 1;
    }
    auto generated = mcpp::manifest::load(root / "mcpp.toml");
    if (!generated) {
        mcpp::ui::error(std::format("generated manifest validation failed: {}", generated.error().format()));
        return 1;
    }
    if (auto committed = tx.commit(); !committed) {
        mcpp::ui::error(committed.error());
        return 1;
    }
    mcpp::ui::line(std::format("Created platform package '{}' at {}, defining {}", project.qualifiedName,
                               tx.final_path().string(), triple));
    mcpp::ui::line(std::format("Next: cd {} && mcpp check platform", project.directoryName));
    return 0;
}

} // namespace mcpp::scaffold
