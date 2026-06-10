// mcpp.cli.cmd_new — CLI parsing + routing for `mcpp new`.
// Implementation lives in mcpp.scaffold.ops.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.cli.cmd_new;

import std;
import mcpplibs.cmdline;
import mcpp.scaffold;
import mcpp.scaffold.ops;
import mcpp.ui;

namespace mcpp::cli {

export int cmd_new(const mcpplibs::cmdline::ParsedArgs& parsed) {
    // Discovery mode: `mcpp new --list-templates <pkg>[@ver]` — no project.
    if (auto lt = parsed.value("list-templates")) {
        return mcpp::scaffold::list_package_templates(mcpp::scaffold::parse_spec(*lt));
    }

    std::string name = parsed.positional(0);
    if (name.empty()) {
        std::println(stderr, "error: `mcpp new` requires a package name (e.g. `mcpp new hello`)");
        return 2;
    }

    // `--template` multi-level SPEC (design v2):
    //   builtin registry (frozen: bin; gui = transitional alias), else a
    //   package template: pkg | pkg:tmpl | pkg@ver | pkg@ver:tmpl.
    std::string tmpl = "bin";
    if (auto t = parsed.value("template")) tmpl = *t;
    if (tmpl == "gui") {
        mcpp::ui::warning(
            "--template gui is deprecated; use `--template imgui` "
            "(the template then ships with — and version-tracks — the library)");
    }
    if (tmpl != "bin" && tmpl != "gui") {
        return mcpp::scaffold::new_from_package_template(name, tmpl);
    }
    return mcpp::scaffold::create_builtin_project(name, /*gui=*/tmpl == "gui");
}

} // namespace mcpp::cli
