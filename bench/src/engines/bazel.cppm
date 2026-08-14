// bench.engines.bazel — Bazel.
//
// Bazel DOES build C++20 named modules — a claim worth stating precisely, because
// the obvious guess is wrong in both directions. Measured on bazel 9.2.0 with
// rules_cc 0.2.22:
//
//   * the `module_interfaces` attribute exists on cc_binary/cc_library, and
//   * it needs BOTH `--experimental_cpp_modules` and `--features=cpp_modules`
//     (each flag's absence produces a different, explicit error), and
//   * with clang it builds and runs; with GCC it dies in bazel's own scanner:
//         aggregate-ddi failed: ... what(): Invalid JSON string
//     i.e. bazel's ddi aggregator cannot parse GCC's P1689 output.
//
// So module support here is CONDITIONAL ON THE COMPILER, which is why supports()
// takes one.
//
// Bazel is also the one engine whose "cold" is genuinely ambiguous. It keeps a
// persistent server and a large action cache outside the workspace, so
// `bazel clean` and `bazel clean --expunge` measure two very different things.
// This adapter uses the non-expunging form and says so in the result note,
// because expunging would also discard the downloaded toolchain — provisioning,
// not building.
export module bench.engines.bazel;

import std;
import bench.protocol;
import bench.spec;
import bench.platform;
import bench.engines.engine;

namespace bench::engines {

class BazelEngine : public Engine {
public:
    std::string_view name() const override;

    Availability probe() const override;

    bool supports(Variant v, std::string_view compiler) const override;

    std::string unsupported_reason(Variant v, std::string_view compiler) const override;

    platform::RunResult configure(const Job&) const override;

    // `bazel build //...` over a package with no rules is a SUCCESS that compiles
    // nothing, in about 0.2s. Ask bazel itself what it is about to build rather
    // than reading the BUILD file here — a hand-rolled rule detector would be one
    // more parser to keep in step with the file it parses.
    std::string unbuildable_reason(const Job& job) const override;

    platform::RunResult build(const Job& job) const override;

    void clean(const Job& job) const override;
};

export std::unique_ptr<Engine> make_bazel();

}  // namespace bench::engines
