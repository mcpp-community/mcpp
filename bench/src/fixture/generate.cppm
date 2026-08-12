// bench.fixture.generate — the same logical project, emitted in three source
// forms.
//
// GENERATED, NOT CHECKED IN, and that is the point. Two hand-written "equivalent"
// projects are almost certainly inequivalent somewhere, and the difference lands
// exactly on the axis being measured. One generator means one definition of what
// the project IS; the variants differ only in how it is spelled.
//
// The three forms:
//   headers      unit_k.hpp declares, unit_k.cpp defines      (the status quo)
//   modules      unit_k.cppm declares AND defines             (what most module
//                                                              code looks like)
//   modules-impl unit_k.cppm declares, unit_k_impl.cpp defines (interface and
//                                                              implementation split)
//
// modules-impl exists to give the "move bodies out of interface units" advice a
// number. What that number IS turns out to depend on the compiler, and an
// earlier version of this comment asserted the opposite of the measurement:
//
//   * GCC 16.1 does NOT put the body of an exported non-template function into
//     the BMI. Editing such a body changes the object file and leaves the BMI
//     byte-identical apart from its embedded timestamps, so an engine that
//     compares BMI CONTENT correctly rebuilds one unit and stops.
//   * An engine that decides from the BMI's mtime cascades anyway, which is why
//     cmake and xmake pay ~10 s for that edit where mcpp pays 0.3 s.
//
// So this variant measures the difference between the two decision rules, not a
// compiler limitation. Templates and inline functions in an interface unit are a
// different story and DO change the BMI — the advice survives, its justification
// is narrower than it was written to be.
export module bench.fixture.generate;

import std;
import bench.protocol;

export namespace bench::fixture {

struct Shape {
    // These defaults ARE the `standard` preset, deliberately: if "no flags" and
    // "--preset standard" produced different fixtures, two people comparing
    // results would have no way to tell which they each ran.
    int units{20};   // how many translation units
    int fanin{3};    // how many earlier units each one depends on → graph depth
    // Distinct template-instantiation blocks per unit. Calibrated, not guessed:
    // each block costs ~0.066 s on top of a 0.38 s floor, so 4 puts a unit at
    // ~0.64 s — the same order as a real project's units (mcpp's are 0.57 s).
    // See detail::support_header() for the measurements behind those numbers.
    int weight{4};
};

// Which files a scenario should perturb. The generator knows the shape, so it
// names them rather than leaving the runner to guess.
struct Targets {
    std::filesystem::path hub;   // depended on by many
    std::filesystem::path leaf;  // depended on by nobody
    std::filesystem::path body;  // holds a function body that can be edited
};

namespace detail {

inline std::string unit_name(int k) { return std::format("unit_{}", k); }

inline std::vector<int> deps_of(int k, const Shape& s) {
    std::vector<int> d;
    for (int j = std::max(0, k - s.fanin); j < k; ++j) d.push_back(j);
    return d;
}

// Body shared by all three variants, so the WORK is identical and only the
// packaging differs. `weight` blocks, each a DISTINCT template instantiation —
// see support_header() for why distinctness is the whole point, and for what
// one block costs.
inline std::string function_body(int k, const Shape& s) {
    std::string b;
    b += "    long long acc = " + std::to_string(k) + ";\n";
    for (int w = 0; w < s.weight; ++w)
        b += std::format("    acc += ::bench_fixture::work<{}>({});\n", w, k + w);
    for (int d : deps_of(k, s))
        b += std::format("    acc += {}_value();\n", unit_name(d));
    b += "    return static_cast<int>(acc & 0x7fffffff);\n";
    return b;
}

// The template the bodies instantiate. Included by EVERY generated unit — in the
// global module fragment for the module variants, directly for the header
// variant — so all three pay the same per-translation-unit cost and differ only
// in how they share declarations.
//
// CALIBRATION, and why this is not the workload it started as. The first version
// measured almost no compilation: a unit cost 0.23 s of which 0.17 s was the
// compiler starting up — 74% process startup — and the `weight` knob barely
// moved it, because it emitted O(weight^2) instantiations of a single trivial
// constexpr recursion (a few hundred at weight 40, which a compiler does in
// microseconds). Measured on gcc 16.1.0, x86_64:
//
//     empty module ................................. 0.17 s
//     old fixture unit, weight 6 ................... 0.23 s
//     one unit with a realistic global module fragment 0.97 s
//     mcpp's own units (57k lines / 139 units) ..... 0.57 s
//
// So the workload is now built from what actually costs time in real C++:
// standard library headers, plus instantiation over DISTINCT types so the
// instantiations cannot be shared between blocks. Cost is 0.38 s + 0.066 s per
// weight unit, which puts the default weight at the same order as a real
// project's units instead of two orders below it.
inline std::string support_header() {
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

// Emits one variant of the project into `root`. Returns the perturbation
// targets for that layout.
Targets emit_sources(const std::filesystem::path& root, Variant variant, const Shape& s);

// ---------------------------------------------------------------------------

inline Targets emit_sources(const std::filesystem::path& root, Variant variant,
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
