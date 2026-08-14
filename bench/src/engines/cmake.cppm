// bench.engines.cmake — CMake + Ninja.
//
// CMake has supported C++20 named modules since 3.28 (with Ninja >= 1.11), so it
// is the reference point for "the mainstream way to build modules today".
module;
// std::sscanf for the version banner; <cstdio> is not reachable through
// `import std;` for the C-library names in the global namespace.
#include <cstdio>

export module bench.engines.cmake;

import std;
import bench.protocol;
import bench.spec;
import bench.platform;
import bench.engines.engine;

namespace bench::engines {

class CMakeEngine : public Engine {
public:
    std::string_view name() const override;

    Availability probe() const override;

    // `import std;` sits behind an experimental gate whose KEY CHANGES WITH THE
    // CMAKE VERSION, and the descriptions in projects/ carry the CMake 4.0 one.
    // An older cmake does not reject the key, it simply does not recognise it —
    // so the gate stays shut and configure dies on the first `import std;` with
    // an error about the standard library rather than about the version.
    //
    // Reporting that as `failed` is what the runner images actually produced:
    // cmake 3.31.6 ships in every GitHub image, and EVERY module cell in the
    // matrix was recorded as a real finding against cmake. It is not one — it is
    // "this engine, at this version, cannot express this cell", which is exactly
    // what `unavailable` plus a reason is for.
    //
    // Only the module forms need it. `headers` builds fine on 3.28, which is why
    // those six cells were the only ones in the whole matrix that ever passed.
    bool supports(Variant v, std::string_view) const override;
    std::string unsupported_reason(Variant v, std::string_view) const override;

    platform::RunResult configure(const Job& job) const override;

    platform::RunResult build(const Job& job) const override;

    // Parsed out of the probe banner ("cmake version 4.0.2"), and cached: the
    // support question is asked once per cell and spawning cmake each time would
    // add a process launch to every row of the matrix.
    struct Version { int major{}; int minor{}; };
    Version version() const;

    // Artifacts only — the configure result lives in the same directory, so a
    // "cold" build here re-runs configure. That is declared in the bench README
    // rather than papered over: cmake genuinely cannot separate the two without
    // keeping a second cache.
    void clean(const Job& job) const override;

private:
    mutable std::optional<Version> version_;
};

export std::unique_ptr<Engine> make_cmake();

}  // namespace bench::engines
