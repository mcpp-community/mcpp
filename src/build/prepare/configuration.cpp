// configuration.cpp -- one configuration prepared without a build (SPEC-013
// §9): `mcpp self env --target` and `mcpp toolchain install --target`.
// Declared in prepare.cppm.

module mcpp.build.prepare;
import :state;

import std;
import mcpp.home;
import mcpp.project;
import mcpp.toolchain.triple;

namespace mcpp::build {

namespace fs = std::filesystem;

std::expected<BuildContext, std::string> prepare_configuration(std::string_view targetTriple) {
    BuildOverrides ov;
    ov.target_triple = std::string(targetTriple);
    std::error_code ec;
    if (auto root = mcpp::project::find_manifest_root(fs::current_path(ec))) {
        ov.project_root = *root;
        return prepare_build(/*print_fingerprint=*/false, /*includeDevDeps=*/false, {}, ov);
    }
    // No project here: an empty program in the home's cache stands for one,
    // one directory per target, so its lock and its build directory persist
    // and a second run finds everything on disk.
    std::string name = "host";
    if (!targetTriple.empty()) {
        auto t = mcpp::toolchain::triple::parse(targetTriple);
        name = t ? t->str() : std::string(targetTriple);
    }
    const auto dir = mcpp::home::root() / "cache" / "configurations" / name;
    fs::create_directories(dir / "src", ec);
    if (ec) return std::unexpected(std::format("cannot create {}: {}", dir.string(), ec.message()));
    auto write_if_absent = [&](const fs::path& p, std::string_view text) {
        if (fs::exists(p, ec)) return true;
        std::ofstream out(p, std::ios::binary);
        out << text;
        return bool(out);
    };
    if (!write_if_absent(dir / "mcpp.toml",
            "# The configuration `mcpp self env --target` and `mcpp toolchain install\n"
            "# --target` prepare when they are run outside a project (written by mcpp).\n"
            "[package]\nname    = \"configuration\"\nversion = \"0.0.0\"\n")
        || !write_if_absent(dir / "src" / "main.cpp", "int main() { return 0; }\n"))
        return std::unexpected(std::format("cannot write the configuration under {}", dir.string()));
    ov.project_root = dir;
    return prepare_build(/*print_fingerprint=*/false, /*includeDevDeps=*/false, {}, ov);
}

} // namespace mcpp::build
