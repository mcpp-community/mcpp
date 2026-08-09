// mcpp.cli.cmd_new — CLI parsing + routing for `mcpp new`.
// Implementation lives in mcpp.scaffold.create.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.cli.cmd_new;

import std;
import mcpplibs.cmdline;
import mcpp.scaffold;
import mcpp.scaffold.create;
import mcpp.ui;

namespace mcpp::cli {

export int cmd_new(const mcpplibs::cmdline::ParsedArgs& parsed) {
    // Discovery mode: `mcpp new --list-templates <pkg>[@ver]` — no project.
    if (auto lt = parsed.value("list-templates")) {
        auto spec = mcpp::scaffold::parse_template_spec(*lt);
        if (!spec) {
            mcpp::ui::error(spec.error().message);
            return 2;
        }
        if (spec->templateName) {
            mcpp::ui::error(
                "--list-templates expects only [ns.]name[@version], without :tname");
            return 2;
        }
        if (spec->legacyList) {
            mcpp::ui::warning(std::format(
                "trailing ':' list syntax is deprecated; use "
                "`mcpp new --list-templates {}`",
                lt->substr(0, lt->size() - 1)));
        }
        return mcpp::scaffold::list_package_templates(*spec);
    }

    std::string name = parsed.positional(0);
    if (name.empty()) {
        std::println(stderr, "error: `mcpp new` requires a package name (e.g. `mcpp new hello`)");
        return 2;
    }

    // `--template` exact package SPEC:
    //   builtin registry (frozen: bin; gui = transitional alias), else a
    //   package template: [ns.]pkg | [ns.]pkg:tmpl | [ns.]pkg@ver:tmpl.
    std::string tmpl = "bin";
    if (auto t = parsed.value("template")) tmpl = *t;
    if (tmpl == "gui") {
        mcpp::ui::warning(
            "--template gui is deprecated; use `--template imgui` "
            "(the template then ships with — and version-tracks — the library)");
    }
    if (tmpl != "bin" && tmpl != "gui") {
        auto spec = mcpp::scaffold::parse_template_spec(tmpl);
        if (!spec) {
            mcpp::ui::error(spec.error().message);
            return 2;
        }
        if (spec->legacyList) {
            mcpp::ui::warning(std::format(
                "trailing ':' list syntax is deprecated; use "
                "`mcpp new --list-templates {}`",
                tmpl.substr(0, tmpl.size() - 1)));
            return mcpp::scaffold::list_package_templates(*spec);
        }
        return mcpp::scaffold::new_from_package_template(name, *spec);
    }
    return mcpp::scaffold::create_builtin_project(name, /*gui=*/tmpl == "gui");
}

} // namespace mcpp::cli
