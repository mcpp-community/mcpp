// bench.fixture.buildfiles — one project description per engine, for one variant.
//
// These files are what makes the comparison fair or meaningless, so each emitter
// pins the same four things: C++23, the same source set, the same optimisation
// level, and one executable. Anything an engine adds beyond that is noted in the
// emitted file itself, so a reader of the fixture can see the asymmetry without
// reading this module.
//
// `import std;` is deliberately ABSENT from every generated project. Engines
// differ wildly in how (and whether) they can build the std module — CMake needs
// a per-version experimental UUID, bazel needs libc++'s std.cppm listed by hand —
// and that difference would dominate the measurement. The fixture reaches the standard
// library through the global module fragment instead, which every engine handles
// identically. The suite measures MODULE MACHINERY, not std-module support.
export module bench.fixture.buildfiles;

import std;
import bench.protocol;
import bench.toolchain;
import bench.fixture.generate;

export namespace bench::fixture {

// Collects the source lists a build description needs, derived from the variant
// rather than by globbing — a generator that guesses its own output is one
// rename away from silently building less than it claims.
struct SourceSet {
    std::vector<std::string> module_interfaces;   // .cppm
    std::vector<std::string> plain_sources;       // .cpp (incl. main + impl units)
};

SourceSet source_set(Variant variant, const Shape& s);

namespace detail {

void write(const std::filesystem::path& p, const std::string& text);

std::string join(const std::vector<std::string>& v, std::string_view sep,
                        std::string_view prefix = "", std::string_view suffix = "");


// Leading space is convenient when concatenating flag strings and wrong inside
// a quoted xmake argument.
std::string trim_copy(std::string_view s);

}  // namespace detail

// --- mcpp -----------------------------------------------------------------

void emit_mcpp(const std::filesystem::path& root, Variant variant, const Shape&,
                      std::string_view compiler = {});

// --- cmake ----------------------------------------------------------------

void emit_cmake(const std::filesystem::path& root, Variant variant, const Shape& s,
                       std::string_view compiler = {});

// --- xmake ----------------------------------------------------------------

void emit_xmake(const std::filesystem::path& root, Variant variant, const Shape&,
                       std::string_view compiler = {});

// --- bazel ----------------------------------------------------------------

void emit_bazel(const std::filesystem::path& root, Variant variant, const Shape& s);

// Emits every build description a fixture instance can need. Engines that do
// not support the variant simply get no file, and their adapter reports
// `unavailable` with a reason rather than failing to find one.
void emit_all(const std::filesystem::path& root, Variant variant, const Shape& s,
                     std::string_view compiler = {});

}  // namespace bench::fixture
