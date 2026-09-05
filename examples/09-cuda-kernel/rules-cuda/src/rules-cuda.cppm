// Compile CUDA device translation units and hand the objects to the link.
//
// WHY A RULE PACKAGE RATHER THAN THE ENGINE
//
// Everything below is knowledge about one vendor's driver: where nvcc lives,
// how it spells an architecture, which host compilers it tolerates, and how it
// must be told about them. None of it is knowledge about the build graph. The
// engine owns the graph, the artifact's identity and the architecture set; the
// spelling of the command that produces the object is this file's business.
//
// The division is not aesthetic. CMake carries nine years of open issues about
// -Xcompiler escaping, response files and device-link edge cases, and every one
// of them is a fact about nvcc that ended up inside a build system.
//
// THE HOST COMPILER IS THE PART THAT SURPRISES PEOPLE
//
// nvcc refuses host compilers newer than a bound it states in its own
// crt/host_config.h, and mcpp's toolchain payload is routinely newer than that
// bound. Passing mcpp's compiler through would fail; passing nothing would let
// nvcc pick the system default and fail the same way. So the rule reads the
// bound and selects a host compiler that satisfies it, and says which one it
// chose — an unexplained third compiler in a build is worse than an error.

export module example.rules.cuda;

import std;
import mcpp;

export namespace example::rules::cuda {

struct options {
    // Device architectures to emit real code for. No default: the set a build
    // compiles for is not the set the machine happens to have, and guessing
    // one produces an artifact that runs here and nowhere else.
    std::vector<std::string> archs;
    // The virtual architecture to embed a portable form of, so newer hardware
    // can JIT. Empty means none, and then the artifact runs only on `archs`.
    std::string              ptx;
    // Header search paths for the island, relative to the package root. The
    // island's own interface lives in one of these, and nvcc is a separate
    // driver that inherits nothing from the C++ side's include configuration.
    std::vector<std::string> includes;
    std::string              out_dir = std::string(mcpp::out_dir());
};

struct edge {
    std::string              id, description;
    std::vector<std::string> command, inputs, outputs;
};

// ─── Locating the toolkit ──────────────────────────────────────────────────

inline std::string first_existing(std::span<const std::string> candidates) {
    for (auto const& c : candidates)
        if (!c.empty() && std::filesystem::exists(c)) return c;
    return {};
}

// The toolkit directory this project declared, or empty.
//
// ⭐ PAYLOAD FIRST, AND THE PROJECT NAMES IT. `mcpp::xpkg_dir` answers for a
// package the manifest declared under `[xlings.workspace]`, which is how a
// build says which toolkit it wants instead of taking whichever one a machine
// happens to have. The 13.x line splits the compiler across components, so the
// pieces are looked up separately and joined here.
inline std::vector<std::string> payload_roots() {
    std::vector<std::string> out;
    for (const char* name : { "cuda-nvcc", "cuda-crt", "cuda-cudart" })
        if (const char* d = mcpp::xpkg_dir("xim", name); d && *d)
            out.emplace_back(d);
    return out;
}

inline std::string find_nvcc() {
    std::vector<std::string> c;
    for (auto const& r : payload_roots()) c.push_back(r + "/bin/nvcc");
    // ⚠️ HOST LOCATIONS ARE LAST AND ARE A FALLBACK, NOT THE DESIGN. A project
    // that declares the payload gets a toolkit whose version it chose and whose
    // host-compiler bound is far newer -- 12.9 accepts gcc 14 and 13.3 accepts
    // gcc 15, where a distribution's CUDA 12.0 stops at 12. These entries exist
    // so a machine that has only a distribution toolkit still builds.
    for (const char* var : { "CUDA_PATH", "CUDA_HOME" })
        if (const char* v = std::getenv(var)) c.push_back(std::string(v) + "/bin/nvcc");
    c.push_back("/usr/local/cuda/bin/nvcc");
    c.push_back("/usr/bin/nvcc");
    return first_existing(c);
}

inline std::string find_host_config(std::string_view nvcc) {
    std::vector<std::string> c;
    // 13.x moved this header into its own component, so the payload that has it
    // is not necessarily the one that has nvcc.
    for (auto const& r : payload_roots()) c.push_back(r + "/include/crt/host_config.h");
    if (!nvcc.empty()) {
        std::filesystem::path p{std::string(nvcc)};
        c.push_back((p.parent_path().parent_path() / "include/crt/host_config.h").string());
    }
    c.push_back("/usr/include/crt/host_config.h");
    return first_existing(c);
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
    if (auto p = text.find("__GNUC__ > "); p != std::string::npos)
        b.gcc = number_after(p + 10);
    if (auto p = text.find("clang version must be less than "); p != std::string::npos)
        if (int excl = number_after(p + 31); excl > 0) b.clang = excl - 1;
    return b;
}

// A host compiler this toolkit accepts, or empty.
//
// Searched newest-first within the bound, because a newer accepted compiler
// produces better host code than an older one and both are equally correct.
inline std::string pick_host_compiler(const bounds& b) {
    for (int v = b.clang ? b.clang : 20; v >= 3; --v) {
        auto p = std::format("/usr/bin/clang++-{}", v);
        if ((b.clang == 0 || v <= b.clang) && std::filesystem::exists(p)) return p;
    }
    for (int v = b.gcc ? b.gcc : 20; v >= 5; --v) {
        auto p = std::format("/usr/bin/g++-{}", v);
        if ((b.gcc == 0 || v <= b.gcc) && std::filesystem::exists(p)) return p;
    }
    return {};
}

// ─── Planning ──────────────────────────────────────────────────────────────

inline std::vector<edge> plan(std::span<const std::string> sources, options opt = {}) {
    std::vector<edge> out;
    const std::string root = mcpp::manifest_dir();
    if (root.empty()) {
        std::println(std::cerr, "example.rules.cuda: no mcpp build context — "
                                "this runs from build.mcpp");
        return out;
    }
    if (opt.archs.empty()) {
        std::println(std::cerr,
            "example.rules.cuda: no architectures given.\n"
            "  The set a build compiles for is not the set this machine has, so\n"
            "  there is no default. Pass e.g. archs = {{\"sm_89\"}}.");
        return out;
    }
    const auto nvcc = find_nvcc();
    if (nvcc.empty()) {
        std::println(std::cerr,
            "example.rules.cuda: no nvcc found (looked at $CUDA_PATH/bin, "
            "/usr/local/cuda/bin, /usr/bin).");
        return out;
    }
    const auto b    = read_bounds(find_host_config(nvcc));
    const auto ccbin = pick_host_compiler(b);
    if (ccbin.empty()) {
        std::println(std::cerr,
            "example.rules.cuda: this toolkit accepts gcc <= {} and clang <= {}, "
            "and no such host compiler is installed.", b.gcc, b.clang);
        return out;
    }
    std::println("example.rules.cuda: nvcc {} with -ccbin {}", nvcc, ccbin);

    // ⭐ THE LINK LINE GETS ITS DIRECTORIES FROM HERE, NOT FROM THE MANIFEST.
    //
    // A manifest that writes `-L/usr/local/cuda/lib64` has decided where the
    // toolkit is, which is the machine's business and not the project's. The
    // rule knows: it just resolved the payload, and it puts that payload's
    // library directory on the link line. The manifest names libraries only.
    //
    // Emitted for every payload root, because the 13.x line splits the runtime
    // out of the compiler and a build may hold both.
    for (auto const& r : payload_roots()) {
        auto lib = r + "/lib";
        if (std::filesystem::is_directory(lib)) mcpp::link_search(lib.c_str());
        // Some components ship `lib64` instead; naming both costs nothing and
        // guessing wrong costs a link error that names a symbol.
        auto lib64 = r + "/lib64";
        if (std::filesystem::is_directory(lib64)) mcpp::link_search(lib64.c_str());
    }

    for (auto const& src : sources) {
        const auto stem = std::filesystem::path(src).stem().string();
        const auto obj  = opt.out_dir + "/" + stem + ".cu.o";
        edge e;
        e.id          = "cuda:" + stem;
        e.description = "nvcc " + src;
        e.command     = { nvcc, "-c", root + "/" + src, "-o", obj,
                          "-ccbin", ccbin, "-std=c++17", "-O2",
                          "--compiler-options", "-fPIC" };
        // ⚠️ THE PAYLOAD'S OWN HEADERS MUST BE NAMED, OR nvcc FINDS THE HOST'S.
        //
        // nvcc adds `<its own dir>/../include` automatically, and on the 12.x
        // line that directory holds `crt/` but NOT `cuda_runtime.h` -- that
        // lives in the `cuda-cudart` component. Without these flags nvcc
        // resolved `cuda_runtime.h` from /usr/include and then read the HOST's
        // `crt/host_config.h` beside it, which on this machine states a bound
        // three major versions older than the payload's. The build failed with
        // the host toolkit's complaint while using the payload's compiler.
        for (auto const& r : payload_roots()) {
            auto inc = r + "/include";
            if (std::filesystem::is_directory(inc)) e.command.push_back("-I" + inc);
        }
        for (auto const& inc : opt.includes)
            e.command.push_back("-I" + root + "/" + inc);
        for (auto const& a : opt.archs) {
            // `compute_NN` is the virtual architecture the real one derives
            // from; nvcc wants both halves named.
            std::string digits;
            for (char c : a) if (std::isdigit(static_cast<unsigned char>(c))) digits += c;
            e.command.push_back("-gencode");
            e.command.push_back(std::format("arch=compute_{},code={}", digits, a));
        }
        if (!opt.ptx.empty()) {
            e.command.push_back("-gencode");
            e.command.push_back(std::format("arch=compute_{0},code=compute_{0}", opt.ptx));
        }
        e.inputs  = { root + "/" + src };
        e.outputs = { obj };
        out.push_back(std::move(e));
    }
    return out;
}

inline bool submit(std::span<const edge> edges) {
    if (edges.empty()) return false;
    for (auto const& e : edges) {
        mcpp::action a;
        a.id          = e.id.c_str();
        // `object`, not `source`: nvcc produces the linkable artifact itself.
        // What the role names is what the output IS, not how it was made.
        a.role        = "object";
        a.description = e.description.c_str();
        for (auto const& c : e.command) a.arg(c.c_str());
        for (auto const& i : e.inputs)  a.input(i.c_str());
        for (auto const& o : e.outputs) a.output(o.c_str());
        a.submit();
    }
    return true;
}

inline bool compile(std::span<const std::string> sources, options opt = {}) {
    auto edges = plan(sources, std::move(opt));
    return submit(edges);
}

} // namespace example::rules::cuda
