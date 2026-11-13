// The case of e2e/runtime that holds the machine (nothing else of this program runs beside it):
// the run-time search closure is payload first, the artifact's own directory next and the SubOS
// farm last, the artifact's DT_RPATH is that closure item by item, and the loader resolves the
// artifact's own library from `$ORIGIN` (formerly tests/e2e/219).
import std;
import mcpplibs.mtest;
import mcpp.test.support;

using namespace mcpplibs::mtest;
namespace s = mcpp::test::support;
namespace fs = std::filesystem;

namespace {

std::optional<fs::path> first(const fs::path& root, std::string_view pattern) {
    const auto all = s::glob(root, pattern);
    if (all.empty()) return std::nullopt;
    return all.front();
}

std::vector<std::string> split(std::string_view text, char sep) {
    std::vector<std::string> out;
    for (auto part : std::views::split(text, sep)) out.emplace_back(part.begin(), part.end());
    return out;
}

E2E machine{"machine", [] {
    "219 runtime search farm is last"_test[s::gcc][s::elf][s::python3][resource{"machine"}] = [] {
        const auto tmp = s::case_dir();
        // A dependency that ships a shared library, so that the program carries `$ORIGIN`.
        s::project greetdep(tmp / "greetdep", {{"mcpp.toml", "[package]\nname = \"greetdep\"\nversion = \"0.1.0\"\n\n[targets.greetdep]\nkind = \"shared\"\n"},
                                               {"src/greetdep.cppm", "export module greetdep;\nexport int greet_value();\n"},
                                               {"src/greetdep.cpp", "module greetdep;\nint greet_value() { return 7; }\n"}});
        s::project proj(tmp / "proj", {{"mcpp.toml", "[package]\nname = \"closure\"\nversion = \"0.1.0\"\n\n[dependencies.greetdep]\npath = \"../greetdep\"\n"},
                                       {"src/main.cpp", "import greetdep;\nint main() { return greet_value() == 7 ? 0 : 1; }\n"}});
        auto r = proj.mcpp({"build"});
        expect(fatal(e2e::succeeded(r))) << s::text(r);
        const auto res = first(proj.at("target"), "*/*/resolution.json");
        const auto bin = first(proj.at("target"), "*/*/bin/closure");
        expect(fatal(res.has_value())) << "FAIL: no resolution.json";
        expect(fatal(bin.has_value())) << "FAIL: no executable";
        // The recorded closure.
        const auto doc = s::json_file(*res);
        const auto& runtime = doc["runtime"];
        const bool has_closure = runtime.contains("search") && runtime["search"].contains("closure") && !runtime["search"]["closure"].is_null();
        expect(fatal(has_closure)) << "FAIL: resolution.json records no runtime search closure";
        std::vector<std::pair<std::string, std::string>> rows;
        for (const auto& d : runtime["search"]["closure"]) rows.emplace_back(d.value("origin", std::string()), d.value("path", std::string()));
        std::string closure;
        for (const auto& [o, p] : rows) closure += o + "\t" + p + "\n";
        std::string farm;
        for (const auto& [o, p] : rows)
            if (o == "subos_farm") farm = p;
        if (farm.empty()) skip("this SubOS exposes no library view, so there is no farm entry to order. Recorded closure:\n" + closure);
        // invariant 1: the closure is ordered, farm last
        expect(fatal(rows.back().first == "subos_farm")) << "FAIL: the last entry of the search closure is '" << rows.back().first << "', not the farm\n" << closure;
        expect(fatal(rows.front().first == "payload")) << "FAIL: the closure does not start with a payload directory (got '" << rows.front().first << "')\n" << closure;
        // invariant 2: the artifact says the same thing (DT_RPATH, read here)
        const auto dt_rpath = s::read_header(*bin).rpath;
        expect(fatal(!dt_rpath.empty())) << "FAIL: executable carries no DT_RPATH";
        const auto entries = split(dt_rpath, ':');
        expect(fatal(entries.back() == farm)) << "FAIL: the farm is not the last entry of DT_RPATH\n      recorded farm: " << farm << "\n      full: " << dt_rpath;
        expect(fatal(e2e::contains(":" + dt_rpath + ":", ":$ORIGIN:"))) << "FAIL: no $ORIGIN in DT_RPATH — this project cannot exercise the ordering it is meant to check. full: " << dt_rpath;
        const auto origin_at = std::ranges::find(entries, "$ORIGIN") - entries.begin();
        const auto farm_at = std::ranges::find(entries, farm) - entries.begin();
        expect(fatal(farm_at < std::ssize(entries))) << "FAIL: farm missing from DT_RPATH entirely";
        expect(fatal(origin_at <= farm_at)) << "FAIL: the SubOS farm outranks $ORIGIN. full: " << dt_rpath;
        // invariant 2b: the record and the artifact agree item by item
        std::vector<std::string> recorded, actual;
        for (const auto& [o, p] : rows) recorded.push_back(o == "artifact" ? "$ORIGIN" : p);
        for (const auto& e : entries)
            if (!e.empty()) actual.push_back(e);
        expect(fatal(recorded == actual)) << "FAIL: the recorded closure and the artifact's DT_RPATH differ\nrecorded:\n" << closure << "DT_RPATH: " << dt_rpath;
        // invariant 3: libc still comes from the payload, not the farm
        const auto rpath_first = entries.front();
        expect(fatal(fs::exists(fs::path(rpath_first) / "libc.so.6"))) << "FAIL: the first DT_RPATH entry holds no libc.so.6, so this case is not measuring the ordering it claims to measure. first: " << rpath_first;
        expect(fatal(e2e::contains(":" + dt_rpath + ":", ":" + farm + ":"))) << "FAIL: farm missing from DT_RPATH entirely";
        expect(fatal(farm_at > 0)) << "FAIL: the farm precedes the payload that supplies libc";
        // invariant 4: the loader agrees, measured rather than inferred
        const auto ran = s::run(*bin, {}, {}, {{"LD_DEBUG", "libs"}});
        expect(fatal(e2e::succeeded(ran))) << "FAIL: the built executable did not run: " << ran.err;
        std::vector<std::string> names;
        for (const auto& e : fs::directory_iterator(bin->parent_path())) names.push_back(e.path().filename().string());
        std::ranges::sort(names);
        std::string soname;
        for (const auto& n : names)
            if (n.starts_with("libgreetdep.so")) { soname = n; break; }
        expect(fatal(!soname.empty())) << "FAIL: no shared library was produced, so invariant 4 is vacuous";
        bool inblock = false;
        std::string consulted;
        for (const auto& line : split(ran.err, '\n')) {
            if (line.contains("find library=" + soname)) { inblock = true; continue; }
            if (inblock && line.contains("find library=")) inblock = false;
            if (inblock && line.contains("trying file=") && line.contains(farm + "/")) consulted += line + "\n";
        }
        expect(fatal(consulted.empty())) << "FAIL: the loader consulted the SubOS farm for " << soname << " before the artifact's own directory. farm: " << farm << "\n" << consulted;
    };
}};

}  // namespace
