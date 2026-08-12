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
// modules-impl exists because of a measured result: with GCC and Clang alike, a
// module interface unit's BMI carries function bodies, so editing ANY body
// cascades to every importer. Moving bodies into implementation units is the
// only fix available (no compiler flag does it — `-fmodules-reduced-bmi` was
// measured and does not). This variant is how that claim gets a number.
export module bench.fixture.generate;

import std;
import bench.protocol;

export namespace bench::fixture {

struct Shape {
    int units{40};   // how many translation units
    int fanin{3};    // how many earlier units each one depends on → graph depth
    int weight{6};   // template instantiations per unit → per-unit compile cost
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
// packaging differs. Templates rather than plain statements: they cost real
// front-end time, which is what a modules benchmark is actually about.
inline std::string function_body(int k, const Shape& s) {
    std::string b;
    b += "    long long acc = " + std::to_string(k) + ";\n";
    for (int w = 0; w < s.weight; ++w) {
        b += std::format(
            "    acc += ::bench_fixture::mix<{}>(std::tuple<int, long, double>{{{}, {}L, {}.0}});\n",
            w, k + w, k * 2 + w, w + 1);
    }
    for (int d : deps_of(k, s))
        b += std::format("    acc += {}_value();\n", unit_name(d));
    b += "    return static_cast<int>(acc & 0x7fffffff);\n";
    return b;
}

// The template the bodies instantiate. Header form for the headers variant,
// global-module-fragment form for the module variants — same code either way.
inline std::string support_header() {
    return R"(#pragma once
#include <tuple>
#include <utility>

namespace bench_fixture {

// A small, deliberately template-heavy helper: each instantiation costs the
// front end real work, which is what makes per-unit compile time non-trivial
// enough to measure. Nothing here is meant to be fast at runtime.
template <int N, typename Tuple>
constexpr long long mix(Tuple t) {
    if constexpr (N <= 0) {
        return static_cast<long long>(std::get<0>(t));
    } else {
        constexpr std::size_t idx = N % std::tuple_size_v<Tuple>;
        return static_cast<long long>(std::get<idx>(t)) + mix<N - 1>(t);
    }
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
