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

std::string unit_name(int k);

std::vector<int> deps_of(int k, const Shape& s);

// Body shared by all three variants, so the WORK is identical and only the
// packaging differs. `weight` blocks, each a DISTINCT template instantiation —
// see support_header() for why distinctness is the whole point, and for what
// one block costs.
std::string function_body(int k, const Shape& s);

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
std::string support_header();

}  // namespace detail

// Emits one variant of the project into `root`. Returns the perturbation
// targets for that layout.
Targets emit_sources(const std::filesystem::path& root, Variant variant, const Shape& s);

// ---------------------------------------------------------------------------

Targets emit_sources(const std::filesystem::path& root, Variant variant,
                            const Shape& s);

}  // namespace bench::fixture
