// Compile CUDA device translation units and hand the objects to the link.
//
// WHY A RULE PACKAGE RATHER THAN THE ENGINE
//
// Everything below is knowledge about one vendor's tools: where the toolkit's
// pieces live, how an architecture is spelled, which host compilers nvcc
// tolerates, how a driver states its version. None of it is knowledge about
// the build graph. The engine owns the graph, the artifact's identity and the
// architecture set (`[build] accel`); the spelling of the command that
// produces an object, and every probe of the machine, is this file's business.
// `tests/unit/test_core_vendor_probes.cpp` in mcpp holds that line from the
// other side: the engine names no vendor tool.
//
// TWO ROUTES, ONE PRIMARY
//
// `clang -x cuda` is the primary route: the resolved toolchain's own clang
// compiles the device unit, so there is no second host compiler and no
// host-compiler bound to satisfy. nvcc is the alternate, taken when the
// project's toolchain is GCC or when asked for: it drives a host compiler
// (`-ccbin`) and refuses one newer than the bound its `crt/host_config.h`
// states, which this rule reads and reports.
//
// WHAT THE RULE TELLS THE ENGINE
//
// Objects, through `mcpp::action` (role "object"); library directories,
// through `mcpp::link_search`; and three claims about the machine that the
// engine compares or relays before the first compile:
//   - the driver's version, read through the driver's own library and stated
//     with `mcpp::fact`, together with the floor the runtime needs
//     (`mcpp::floor`); an unmet floor refuses the build with both values;
//   - whether nvcc can reach its own back-end stages (`--dryrun`), as an
//     advisory naming the first stage that does not resolve;
//   - whether the embedded PTX can be JIT-compiled by this driver, as an
//     advisory, because the SASS for the named architectures still runs.

module;
#include <cstdlib>
#include <cstdio>
#if !defined(_WIN32)
#include <dlfcn.h>
#endif

export module mcpplibs.rules.cuda;

import std;
import mcpp;

export namespace mcpplibs::rules::cuda {

enum class route { automatic, clang, nvcc };

struct options {
    route       which   = route::automatic;
    // Header search paths for the island. Relative entries resolve against the
    // package root; an ABSOLUTE entry is passed through unchanged.
    //
    // ⭐ THE ABSOLUTE FORM IS FOR A DEPENDENCY'S HEADERS. A device compiler is
    // a separate driver and inherits nothing from the C++ side's include
    // configuration, so a package whose device code includes a dependency's
    // header -- ggml's CUDA backend includes `cublas_v2.h` -- has to name that
    // dependency's directory here, and it knows it only as the absolute path
    // `mcpp::dep_dir` answered with.
    std::vector<std::string> includes;
    std::string out_dir = std::string(mcpp::out_dir());
};

// ─── What the engine said ──────────────────────────────────────────────────

// The `cuda` chunk of `mcpp::accel()`, in this rule's own reading: the engine
// carries the string and compares it as a shape; what `sm_89` means is ours.
struct target {
    std::string version;                 // "12.9"
    std::vector<std::string> archs;      // {"sm_89"}
    std::string ptx;                     // "89" when a portable form is embedded
    bool present = false;
};

// Split on one character. Written out rather than taken from <ranges>: GCC 16
// refuses the ranges split view instantiated inside an exported inline function
// when build.mcpp imports this module (`conflicting deduced return type for
// imported declaration ... view_interface::data()`), and clang does not.
inline std::vector<std::string_view> split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    for (std::size_t i = 0; i <= s.size();) {
        auto j = s.find(sep, i);
        out.push_back(s.substr(i, j == std::string_view::npos ? s.size() - i : j - i));
        if (j == std::string_view::npos) break;
        i = j + 1;
    }
    return out;
}

inline std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t')) s.remove_suffix(1);
    return s;
}

inline target parse_target(std::string_view accel) {
    target t;
    for (std::size_t i = 0; i <= accel.size();) {
        auto comma = accel.find(',', i);
        auto open = accel.find('{', i), close = accel.find('}', i);
        if (open != std::string_view::npos && close != std::string_view::npos
            && comma != std::string_view::npos && comma > open && comma < close)
            comma = accel.find(',', close);
        auto chunk = trim(comma == std::string_view::npos ? accel.substr(i)
                                                          : accel.substr(i, comma - i));
        i = comma == std::string_view::npos ? accel.size() + 1 : comma + 1;
        if (!chunk.starts_with("cuda")) continue;
        t.present = true;
        auto plus = chunk.find('+');
        t.version = std::string(trim(chunk.substr(4, plus == std::string_view::npos
                                                       ? chunk.size() - 4 : plus - 4)));
        if (plus != std::string_view::npos) {
            auto o = chunk.find('{', plus), c = chunk.find('}', plus);
            if (o != std::string_view::npos && c != std::string_view::npos)
                for (auto part : split(chunk.substr(o + 1, c - o - 1), ','))
                    if (auto a = trim(part); !a.empty()) t.archs.emplace_back(a);
            auto tail = chunk.substr(c == std::string_view::npos ? chunk.size() : c + 1);
            for (auto key : {"ptx>=", "floor>="})
                if (auto p = tail.find(key); p != std::string_view::npos)
                    t.ptx = std::string(trim(tail.substr(p + std::string_view(key).size())));
        }
    }
    return t;
}

inline std::vector<std::string> device_sources() {
    std::vector<std::string> out;
    for (auto part : split(std::string_view(mcpp::device_sources()), '\n'))
        if (auto s = trim(part); !s.empty()) out.emplace_back(s);
    return out;
}

// ─── Locating the toolkit ──────────────────────────────────────────────────

// The toolkit this project declared under `[xlings.workspace]`, by component.
// The 13.x line splits the compiler across `cuda-nvcc`, `cuda-crt` and
// `libnvvm`; the 12.x line keeps them in `cuda-nvcc`. Either way the project
// names the compiler and this rule finds the pieces.
struct toolkit {
    std::string nvcc_root, cudart_root, crt_root, driver_dir;
    std::string nvcc() const { return nvcc_root + "/bin/nvcc"; }
    std::string host_config() const {
        for (auto const* r : { &crt_root, &nvcc_root, &cudart_root }) {
            if (r->empty()) continue;
            auto p = *r + "/include/crt/host_config.h";
            if (std::filesystem::exists(p)) return p;
        }
        return {};
    }
    std::vector<std::string> include_dirs() const {
        std::vector<std::string> out;
        for (auto const* r : { &cudart_root, &crt_root, &nvcc_root })
            if (!r->empty() && std::filesystem::is_directory(*r + "/include"))
                out.push_back(*r + "/include");
        return out;
    }
    std::vector<std::string> lib_dirs() const {
        std::vector<std::string> out;
        for (auto const* r : { &cudart_root, &nvcc_root })
            for (auto const* sub : { "/lib", "/lib64" })
                if (!r->empty() && std::filesystem::is_directory(*r + sub))
                    out.push_back(*r + sub);
        return out;
    }
};

inline std::string xpkg(const char* name) {
    if (const char* d = mcpp::xpkg_dir("xim", name); d && *d) return d;
    return {};
}

inline std::optional<toolkit> find_toolkit() {
    toolkit t;
    t.nvcc_root   = xpkg("cuda-nvcc");
    t.cudart_root = xpkg("cuda-cudart");
    t.crt_root    = xpkg("cuda-crt");
    t.driver_dir  = xpkg("libcuda-host-link");
    if (t.nvcc_root.empty() || t.cudart_root.empty()) {
        std::println(std::cerr,
            "mcpplibs.rules.cuda: the toolkit is not declared.\n"
            "  Name it under [xlings.workspace] and mcpp provisions it on first use:\n"
            "    \"xim:cuda-nvcc\"   = \"12.9.86\"\n"
            "    \"xim:cuda-cudart\" = \"12.9.79\"\n"
            "  (found nvcc: '{}', cudart: '{}')", t.nvcc_root, t.cudart_root);
        return std::nullopt;
    }
    return t;
}

// ─── Probes: what the machine has, what the toolkit needs ──────────────────

// The driver's version through the driver's own library, reached through the
// sentinel package rather than /usr/lib. "" when there is no driver here,
// which is a fact about the machine and not a failure of the build.
inline std::string driver_version(const toolkit& t) {
#if defined(_WIN32)
    return {};
#else
    if (t.driver_dir.empty()) return {};
    const auto lib = t.driver_dir + "/lib/libcuda.so.1";
    if (!std::filesystem::exists(lib)) return {};
    // What would change the answer is the library the answer was read from.
    mcpp::rerun_if_changed(lib.c_str());
    void* h = ::dlopen(lib.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (!h) {
        const char* why = ::dlerror();
        mcpp::warning(std::format("could not open the driver library {}: {}", lib,
                                  why ? why : "(no reason given)").c_str());
        return {};
    }
    using fn = int (*)(int*);
    auto get = reinterpret_cast<fn>(::dlsym(h, "cuDriverGetVersion"));
    int v = 0;
    std::string out;
    if (get && get(&v) == 0 && v > 0) out = std::format("{}.{}", v / 1000, (v % 1000) / 10);
    ::dlclose(h);
    return out;
#endif
}

inline int major_of(std::string_view v) {
    int m = 0;
    for (char c : v) { if (!std::isdigit(static_cast<unsigned char>(c))) break; m = m * 10 + (c - '0'); }
    return m;
}

inline bool version_at_least(std::string_view have, std::string_view want) {
    auto parse = [](std::string_view s) {
        std::vector<int> out; int acc = 0; bool digits = false;
        for (char c : s) {
            if (c == '.') { out.push_back(acc); acc = 0; digits = false; continue; }
            if (!std::isdigit(static_cast<unsigned char>(c))) break;
            acc = acc * 10 + (c - '0'); digits = true;
        }
        if (digits) out.push_back(acc);
        return out;
    };
    auto h = parse(have), w = parse(want);
    for (std::size_t i = 0; i < std::max(h.size(), w.size()); ++i) {
        int a = i < h.size() ? h[i] : 0, b = i < w.size() ? w[i] : 0;
        if (a != b) return a > b;
    }
    return true;
}

// State the driver relation. The engine compares the floor against the fact
// and refuses with both values; this rule only knows which numbers matter.
//
// The floor is the toolkit's major: a 12.x runtime runs on any 12.x driver
// (minor-version compatibility), and fails at the first allocation on an 11.x
// one. The embedded PTX is a separate, softer question: PTX emitted by toolkit
// 12.9 is JIT-compiled only by a driver at or above 12.9, but the SASS for the
// named architectures still runs, so a driver below the toolkit costs reach on
// newer hardware rather than correctness here -- reported, not enforced.
inline void state_driver_relation(const toolkit& t, const target& tg) {
    const auto driver = driver_version(t);
    if (!driver.empty()) mcpp::fact("cuda.driver", driver.c_str());
    else mcpp::warning("no driver library reachable through xim:libcuda-host-link; "
                       "the build proceeds and the artifact will find no device at run time");
    const int major = major_of(tg.version);
    if (major > 0) mcpp::floor(std::format("cuda.driver >= {}.0", major).c_str());
    if (!driver.empty() && !tg.ptx.empty() && !version_at_least(driver, tg.version))
        mcpp::warning(std::format(
            "the PTX embedded for compute_{} was emitted by toolkit {} and this driver "
            "serves {}; hardware newer than {{{}}} will not be able to JIT it. The named "
            "architectures run. Build with a toolkit at or below the driver, or add the "
            "newer hardware's SASS to [build] accel.",
            tg.ptx, tg.version, driver, [&] {
                std::string s; for (auto& a : tg.archs) { if (!s.empty()) s += ','; s += a; }
                return s; }()).c_str());
}

// The greatest gcc major and the greatest clang major the toolkit accepts.
// Zero means the header said nothing, which is not a refusal.
struct bounds { int gcc = 0, clang = 0; };

inline bounds read_bounds(std::string_view headerPath) {
    bounds b;
    if (headerPath.empty()) return b;
    std::ifstream in{std::string(headerPath)};
    std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    auto number_after = [&](std::size_t pos) {
        int v = 0, n = 0;
        while (pos < text.size() && !std::isdigit(static_cast<unsigned char>(text[pos]))) {
            if (text[pos] == '\n') return 0;
            ++pos;
        }
        while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) {
            v = v * 10 + (text[pos] - '0'); ++pos; ++n;
        }
        return n ? v : 0;
    };
    if (auto p = text.find("__GNUC__ > "); p != std::string::npos) b.gcc = number_after(p + 10);
    if (auto p = text.find("clang version must be less than "); p != std::string::npos)
        if (int excl = number_after(p + 31); excl > 0) b.clang = excl - 1;
    return b;
}

// Does the C library this build compiles against declare the C23 functions
// `cospi`, `sinpi` and `rsqrt`?
//
// ⚠️ Measured 2026-09-05 against glibc 2.44. Toolkit 12.9's
// `crt/math_functions.h` declares those same names for the host WITHOUT
// `noexcept`; glibc declares them WITH it, and since C++17 that is part of the
// function type. nvcc's front end stops with six `exception specification is
// incompatible` errors that name a glibc header and a CUDA header and leave
// the reader to work out that neither is at fault alone. The 13.x line does
// not redeclare them and compiles cleanly against the same C library.
//
// Read, not probed. The answer is one substring of one header the sysroot
// already contains; a probe compile would spend a second nvcc invocation to
// learn the same thing, and would report it as a compile failure rather than
// as a pairing that cannot work.
inline bool libc_declares_c23_pi_math(std::string_view sysroot) {
    if (sysroot.empty()) return false;
    for (auto const* rel : { "/usr/include/bits/mathcalls.h", "/include/bits/mathcalls.h" }) {
        std::ifstream in{std::string(sysroot) + rel};
        if (!in) continue;
        std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        return text.find("(cospi,") != std::string::npos
            && text.find("(rsqrt,") != std::string::npos;
    }
    return false;
}

// `<compiler> -dumpversion` → major. The host compiler is the toolchain mcpp
// resolved for this build, so its version is a fact of the build, not a guess.
inline int compiler_major(const std::string& cc) {
#if defined(_WIN32)
    (void)cc; return 0;
#else
    std::string cmd = cc + " -dumpversion 2>/dev/null";
    if (FILE* p = ::popen(cmd.c_str(), "r")) {
        char buf[64] = {};
        std::string s;
        if (std::fgets(buf, sizeof buf, p)) s = buf;
        ::pclose(p);
        return major_of(s);
    }
    return 0;
#endif
}

// The first back-end stage nvcc names but cannot resolve, from its own plan.
// nvcc invokes cicc, cudafe++, ptxas and fatbinary by bare name on a PATH it
// states in the plan; a stage that does not resolve there fails the compile
// with `sh: 1: cicc: not found`, naming nothing that helps.
inline std::optional<std::string> unreachable_stage(const toolkit& t, const std::string& ccbin) {
#if defined(_WIN32)
    (void)t; (void)ccbin; return std::nullopt;
#else
    const auto probe = std::filesystem::temp_directory_path() / "mcpp-rules-cuda-dryrun.cu";
    { std::ofstream(probe) << "__global__ void k() {}\n"; }
    std::string cmd = std::format("{} --dryrun -ccbin {} -c {} -o /dev/null 2>&1",
                                  t.nvcc(), ccbin, probe.string());
    std::string text;
    if (FILE* p = ::popen(cmd.c_str(), "r")) {
        char buf[4096];
        while (std::fgets(buf, sizeof buf, p)) text += buf;
        ::pclose(p);
    }
    std::filesystem::remove(probe);
    std::string path;
    std::vector<std::string> stages;
    for (auto l : split(text, '\n')) {
        if (!l.starts_with("#$ ")) continue;
        l.remove_prefix(3);
        if (l.starts_with("PATH=")) { path = std::string(l.substr(5)); continue; }
        for (auto const* stage : { "cicc", "cudafe++", "ptxas", "fatbinary", "nvlink" }) {
            auto pos = l.find(stage);
            if (pos == 0 || (pos != std::string_view::npos && (l[pos - 1] == ' ' || l[pos - 1] == '"')))
                if (std::ranges::find(stages, stage) == stages.end()) stages.emplace_back(stage);
        }
    }
    if (stages.empty()) return std::nullopt;      // no plan, no finding
    for (auto const& stage : stages) {
        bool found = false;
        for (auto dir : split(path, ':')) {
            std::string d(dir);
            if (!d.empty() && std::filesystem::exists(d + "/" + stage)) { found = true; break; }
        }
        if (!found) return stage;
    }
    return std::nullopt;
#endif
}

// ─── Planning ──────────────────────────────────────────────────────────────

struct edge {
    std::string id, description;
    std::vector<std::string> command, inputs, outputs;
};

inline route decide(route asked) {
    if (asked != route::automatic) return asked;
    return std::string_view(mcpp::compiler()) == "clang" ? route::clang : route::nvcc;
}

inline std::vector<edge> plan(std::span<const std::string> sources, options opt = {}) {
    std::vector<edge> out;
    const std::string root = mcpp::manifest_dir();
    if (root.empty()) {
        std::println(std::cerr, "mcpplibs.rules.cuda: no mcpp build context -- this runs from build.mcpp");
        return out;
    }
    const auto tg = parse_target(mcpp::accel());
    if (!tg.present || tg.archs.empty()) {
        // C19: a device build that names no device is refused HERE, not at
        // run time as `no kernel image is available for execution`.
        std::println(std::cerr,
            "mcpplibs.rules.cuda: [build] accel names no CUDA architecture (accel = \"{}\").\n"
            "  Write e.g.  accel = \"cuda12.9+{{sm_89}} ptx>=89\"  -- the set a build compiles\n"
            "  for is a decision, and the machine's own hardware is a poor default for it.",
            mcpp::accel());
        return out;
    }
    auto tk = find_toolkit();
    if (!tk) return out;
    state_driver_relation(*tk, tg);

    const route r = decide(opt.which);
    const std::string tcdir = mcpp::toolchain_dir();
    std::string driver_cc;          // the compiler that runs the device unit
    std::vector<std::string> front; // the command up to the input file
    if (r == route::clang) {
        driver_cc = tcdir + "/bin/clang++";
        if (!std::filesystem::exists(driver_cc)) {
            std::println(std::cerr, "mcpplibs.rules.cuda: the clang route needs the toolchain's clang++ at {}", driver_cc);
            return out;
        }
        front = { driver_cc, "-x", "cuda", "-std=c++17", "-O2", "-fPIC",
                  "--cuda-path=" + tk->nvcc_root, "-Wno-unknown-cuda-version",
                  // ⚠️ NVIDIA'S HEADER REFUSES libc++, AND THE REFUSAL IS
                  // ABOUT nvcc RATHER THAN ABOUT THIS COMPILER.
                  //
                  //   crt/host_defines.h:67: error: "libc++ is not supported
                  //   on x86 system"
                  //
                  // The guard is `#if defined(__CUDACC__) && … &&
                  // defined(_LIBCPP_VERSION)`, and clang defines `__CUDACC__`
                  // when it compiles CUDA itself — so a device unit that
                  // includes <cuda_runtime.h> stops here on any LLVM
                  // toolchain, which is the toolchain this route exists for.
                  // Measured on ggml's CUDA backend; the CUDA example's own
                  // kernel never showed it because a bare kernel includes no
                  // toolkit header at all.
                  //
                  // The escape hatch is upstream's own, and it is passed only
                  // on this route: nvcc's host pass really does break against
                  // libc++, and nothing here weakens that.
                  "-D_ALLOW_UNSUPPORTED_LIBCPP" };
        for (auto const& inc : tk->include_dirs()) front.push_back("-I" + inc);
        for (auto const& a : tg.archs) front.push_back("--cuda-gpu-arch=" + a);
        // clang checks ptxas and fatbinary itself; say so before it does.
        for (auto const* tool : { "ptxas", "fatbinary" })
            if (!std::filesystem::exists(tk->nvcc_root + "/bin/" + tool))
                mcpp::warning(std::format("the toolkit payload has no {}; clang invokes it "
                                          "after generating PTX", tool).c_str());
        std::println("mcpplibs.rules.cuda: clang route -- {} (toolkit {})", driver_cc, tk->nvcc_root);
    } else {
        // nvcc drives the toolchain's own compiler, and refuses one newer than
        // the bound its header states. Read the bound; if exceeded, pass the
        // escape hatch and say so -- an unexplained flag is worse than a note.
        const bool clangHost = std::string_view(mcpp::compiler()) == "clang";
        if (clangHost) {
            // Measured: nvcc's own crt/host_defines.h stops the compile with
            // `libc++ is not supported on x86 system`, and libc++ is what an
            // LLVM toolchain's clang uses. The pairing that works is nvcc with
            // a GCC toolchain; with an LLVM toolchain the clang route is the
            // one to take, and it is the default.
            std::println(std::cerr,
                "mcpplibs.rules.cuda: the nvcc route needs a GCC host compiler; this project's "
                "toolchain is LLVM, whose clang uses libc++ and nvcc refuses it. Use the clang "
                "route (the default for an LLVM toolchain) or set [toolchain] to a gcc payload.");
            return out;
        }
        // The other pairing this route cannot have: an old toolkit and a C
        // library new enough to have the C23 `pi` functions. Stated before the
        // compile, because the compile's own report names two headers and no
        // decision.
        if (major_of(tg.version) < 13
            && libc_declares_c23_pi_math(mcpp::toolchain_sysroot())) {
            std::println(std::cerr,
                "mcpplibs.rules.cuda: toolkit {} redeclares the C23 functions cospi, sinpi and "
                "rsqrt for the host without `noexcept`, and the C library this build compiles "
                "against declares them with it; nvcc's front end refuses the pair.\n"
                "  Name a 13.x toolkit, whose headers leave them to the C library:\n"
                "    [xlings.workspace]\n"
                "    \"xim:cuda-nvcc\"   = \"13.3.33\"\n"
                "    \"xim:cuda-crt\"    = \"13.3.33\"\n"
                "    \"xim:cuda-cudart\" = \"13.3.29\"\n"
                "  or take the clang route, which does not include that header at all.",
                tg.version);
            return out;
        }
        // The host compiler nvcc drives, chosen within the bound the toolkit
        // states. Measured: gcc 16 under nvcc 12.9 (bound gcc <= 14) fails inside
        // nvcc's front end on GCC 16's own <type_traits> even with
        // -allow-unsupported-compiler -- the escape hatch admits a compiler one
        // step past the bound, not a standard library two majors newer. So the
        // rule does not guess: the toolchain's g++ when it is within the bound,
        // otherwise a gcc payload the project declared for this purpose, and
        // otherwise a refusal that says which declaration to add.
        const auto b = read_bounds(tk->host_config());
        const std::string tcGcc = tcdir + "/bin/g++";
        const int tcMajor = compiler_major(tcGcc);
        if (b.gcc == 0 || tcMajor <= b.gcc) {
            driver_cc = tcGcc;
        } else if (auto payload = xpkg("gcc"); !payload.empty()
                   && compiler_major(payload + "/bin/g++") <= b.gcc) {
            driver_cc = payload + "/bin/g++";
            mcpp::warning(std::format(
                "nvcc {} states gcc <= {} in {}; the toolchain's gcc {} exceeds it, so the "
                "device unit is compiled with the declared xim:gcc payload ({}). The clang "
                "route has no such bound.", tg.version, b.gcc, tk->host_config(), tcMajor,
                driver_cc).c_str());
        } else {
            std::println(std::cerr,
                "mcpplibs.rules.cuda: nvcc {} accepts gcc <= {} ({}), and this project's "
                "toolchain is gcc {}.\n"
                "  Declare a gcc payload within the bound and the rule drives that one:\n"
                "    [xlings.workspace]\n"
                "    \"xim:gcc\" = \"13.3.0\"\n"
                "  or take the clang route with [toolchain] default = \"llvm@22.1.8\".",
                tg.version, b.gcc, tk->host_config(), tcMajor);
            return out;
        }
        front = { tk->nvcc(), "-ccbin", driver_cc, "-std=c++17", "-O2",
                  "--compiler-options", "-fPIC" };
        // The host compiler nvcc drives is not one mcpp resolved, so nothing
        // has told it where the C library or the assembler are. Measured: with
        // neither of these, NVIDIA's own crt/host_config.h stops at
        // `features.h: No such file or directory`. Both are the flags mcpp
        // passes to its own compiler for this target.
        if (const char* sr = mcpp::toolchain_sysroot(); sr && *sr) {
            front.push_back("--compiler-options");
            front.push_back(std::string("--sysroot=") + sr);
        }
        if (const char* bu = mcpp::toolchain_binutils_dir(); bu && *bu) {
            front.push_back("--compiler-options");
            front.push_back(std::string("-B") + bu);
        }
        for (auto const& inc : tk->include_dirs()) front.push_back("-I" + inc);
        for (auto const& a : tg.archs) {
            std::string digits;
            for (char c : a) if (std::isdigit(static_cast<unsigned char>(c))) digits += c;
            front.push_back("-gencode");
            front.push_back(std::format("arch=compute_{},code={}", digits, a));
        }
        if (!tg.ptx.empty()) {
            front.push_back("-gencode");
            front.push_back(std::format("arch=compute_{0},code=compute_{0}", tg.ptx));
        }
        if (auto missing = unreachable_stage(*tk, driver_cc))
            mcpp::warning(std::format(
                "nvcc cannot reach its own back-end: it invokes '{}' by name and that name "
                "does not resolve on the search path it states. On the 13.x line install "
                "xim:libnvvm beside xim:cuda-nvcc.", *missing).c_str());
        std::println("mcpplibs.rules.cuda: nvcc route -- {} with -ccbin {}", tk->nvcc(), driver_cc);
    }

    // The link line gets its directories from here, not from the manifest: the
    // rule resolved the payload, so the rule names where its libraries are.
    for (auto const& d : tk->lib_dirs()) mcpp::link_search(d.c_str());

    for (auto const& src : sources) {
        const auto stem = std::filesystem::path(src).stem().string();
        const auto obj  = opt.out_dir + "/" + stem + ".cu.o";
        edge e;
        e.id          = "cuda:" + stem;
        e.description = (r == route::clang ? "clang -x cuda " : "nvcc ") + src;
        e.command     = front;
        for (auto const& inc : opt.includes)
            e.command.push_back("-I" + (std::filesystem::path(inc).is_absolute()
                                        ? inc : root + "/" + inc));
        e.command.insert(e.command.end(), { "-c", root + "/" + src, "-o", obj });
        e.inputs  = { root + "/" + src };
        e.outputs = { obj };
        out.push_back(std::move(e));
    }
    return out;
}

inline bool submit(std::span<const edge> edges) {
    for (auto const& e : edges) {
        mcpp::action a;
        a.id          = e.id.c_str();
        a.role        = "object";     // the linkable artifact itself
        a.description = e.description.c_str();
        for (auto const& c : e.command) a.arg(c.c_str());
        for (auto const& i : e.inputs)  a.input(i.c_str());
        for (auto const& o : e.outputs) a.output(o.c_str());
        a.submit();
    }
    return true;
}

// Everything from the manifest: the architectures from `[build] accel`, the
// sources from the constrained glob in `[build] sources`. A build that asks
// for no accelerator has no device sources and nothing to do here -- that is
// the CPU-only variant, and the seam's fallback carries it.
inline bool compile(options opt = {}) {
    if (!*mcpp::accel()) return true;
    const auto sources = device_sources();
    if (sources.empty()) {
        mcpp::warning("[build] accel names a device but no constrained glob matched a device "
                      "source; nothing was compiled for it");
        return true;
    }
    auto edges = plan(sources, std::move(opt));
    if (edges.empty()) return false;
    return submit(edges);
}

} // namespace mcpplibs::rules::cuda
