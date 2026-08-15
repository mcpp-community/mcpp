// bench.fixture.generate — implementation.
//
// `module bench.fixture.generate;` with no `export`: an implementation unit, so nothing below
// reaches an importer's BMI. The interface keeps `Shape` and `Targets` — the
// vocabulary the runner and the buildfile emitters share — and nothing else.
module bench.fixture.generate;

import std;
import bench.protocol;

namespace bench::fixture {

namespace detail {

std::string unit_name(int k) { return std::format("unit_{}", k); }

std::vector<int> deps_of(int k, const Shape& s) {
    std::vector<int> d;
    for (int j = std::max(0, k - s.fanin); j < k; ++j) d.push_back(j);
    return d;
}

std::string function_body(int k, const Shape& s) {
    std::string b;
    b += "    long long acc = " + std::to_string(k) + ";\n";
    for (int w = 0; w < s.weight; ++w)
        b += std::format("    acc += ::bench_fixture::work<{}>({});\n", w, k + w);
    for (int d : deps_of(k, s))
        b += std::format("    acc += {}_value();\n", unit_name(d));
    b += "    return static_cast<int>(acc & 0x7fffffff);\n";
    return b;
}

std::string support_header() {
    return R"(#pragma once
#include <algorithm>
#include <map>
#include <numeric>
#include <string>
#include <vector>

namespace bench_fixture {

// The tag makes every `work<N>` a distinct instantiation of map, vector, string
// and sort. Without it the compiler instantiates one set and every later block
// is free — which is precisely why the previous knob did nothing.
template <int Tag>
struct Key {
    int v;
    friend bool operator<(const Key& a, const Key& b) { return a.v < b.v; }
};

template <int Tag>
long long work(int seed) {
    std::map<Key<Tag>, std::vector<std::string>> m;
    for (int i = 0; i < 4; ++i)
        m[Key<Tag>{seed + i}].push_back(std::to_string(seed * i));
    std::vector<int> v;
    v.reserve(8);
    for (const auto& [k, strs] : m) v.push_back(static_cast<int>(k.v + strs.size()));
    std::sort(v.begin(), v.end());
    return std::accumulate(v.begin(), v.end(), 0LL);
}

}  // namespace bench_fixture
)";
}

}  // namespace detail

Targets emit_sources(const std::filesystem::path& root, Variant variant,
                            const Shape& s) {
    namespace fs = std::filesystem;
    using detail::unit_name;
    using detail::deps_of;

    fs::create_directories(root / "src");
    if (variant == Variant::Headers) fs::create_directories(root / "include");

    auto write = [](const fs::path& p, const std::string& text) {
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out << text;
    };

    // The support template lives in a header for every variant. In the module
    // variants it is pulled in through the global module fragment, which is
    // exactly how real module code reaches legacy headers — keeping it means
    // the fixture exercises that path instead of pretending it does not exist.
    write(root / (variant == Variant::Headers ? "include/fixture_support.hpp"
                                              : "src/fixture_support.hpp"),
          detail::support_header());

    for (int k = 0; k < s.units; ++k) {
        const auto name = unit_name(k);
        const auto deps = deps_of(k, s);

        if (variant == Variant::Headers) {
            std::string hpp = "#pragma once\n";
            for (int d : deps) hpp += std::format("#include \"{}.hpp\"\n", unit_name(d));
            hpp += std::format("\nint {}_value();\n", name);
            write(root / "include" / (name + ".hpp"), hpp);

            std::string cpp = std::format("#include \"{}.hpp\"\n", name);
            cpp += "#include \"fixture_support.hpp\"\n\n";
            cpp += std::format("int {}_value() {{\n{}}}\n", name, detail::function_body(k, s));
            write(root / "src" / (name + ".cpp"), cpp);

        } else {
            std::string ixx = "module;\n#include \"fixture_support.hpp\"\n\n";
            ixx += std::format("export module fx.{};\n\n", name);
            for (int d : deps) ixx += std::format("import fx.{};\n", unit_name(d));
            ixx += "\n";

            if (variant == Variant::Modules) {
                // Body IN the interface unit — the shape most module code takes,
                // and the one whose BMI churns on every edit.
                ixx += std::format("export int {}_value() {{\n{}}}\n", name,
                                   detail::function_body(k, s));
            } else {
                // Declaration only; the definition goes to an implementation
                // unit, which produces no BMI at all.
                ixx += std::format("export int {}_value();\n", name);
                std::string impl = "module;\n#include \"fixture_support.hpp\"\n\n";
                impl += std::format("module fx.{};\n\n", name);
                impl += std::format("int {}_value() {{\n{}}}\n", name,
                                    detail::function_body(k, s));
                write(root / "src" / (name + "_impl.cpp"), impl);
            }
            write(root / "src" / (name + ".cppm"), ixx);
        }
    }

    // main pulls the last unit, which transitively reaches everything.
    const int last = s.units - 1;
    std::string main_cpp;
    if (variant == Variant::Headers) {
        main_cpp = std::format("#include \"{}.hpp\"\n#include <cstdio>\n\n"
                               "int main() {{ std::printf(\"%d\\n\", {}_value()); return 0; }}\n",
                               unit_name(last), unit_name(last));
    } else {
        main_cpp = std::format("#include <cstdio>\nimport fx.{};\n\n"
                               "int main() {{ std::printf(\"%d\\n\", {}_value()); return 0; }}\n",
                               unit_name(last), unit_name(last));
    }
    write(root / "src" / "main.cpp", main_cpp);

    // hub  = unit 0: every other unit reaches it transitively, so an interface
    //        change there is the worst case for cascades.
    // leaf = the last unit: only main depends on it.
    // body = the hub too, because "edit a body in the most-depended-on unit" is
    //        the scenario that separates the three variants most sharply.
    Targets t;
    if (variant == Variant::Headers) {
        t.hub  = root / "include" / (unit_name(0) + ".hpp");
        t.leaf = root / "src" / (unit_name(last) + ".cpp");
        t.body = root / "src" / (unit_name(0) + ".cpp");
    } else if (variant == Variant::Modules) {
        t.hub  = root / "src" / (unit_name(0) + ".cppm");
        t.leaf = root / "src" / (unit_name(last) + ".cppm");
        t.body = root / "src" / (unit_name(0) + ".cppm");
    } else {
        t.hub  = root / "src" / (unit_name(0) + ".cppm");
        t.leaf = root / "src" / (unit_name(last) + ".cppm");
        t.body = root / "src" / (unit_name(0) + "_impl.cpp");
    }
    return t;
}

}  // namespace bench::fixture
