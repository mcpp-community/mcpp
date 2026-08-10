// mcpp.pack.host_requirements — what a distributed artifact needs the TARGET
// machine to provide, and how that list is spelled.
//
// WHY THIS EXISTS
//
// "Self-contained" has a floor. A graphics program's driver — the vendor
// user-space half — cannot be bundled: it is version-locked to the running
// kernel module, and for the proprietary stacks redistribution is forbidden
// outright. So the honest output of packaging such a program is not a bundle
// that quietly omits it, but a bundle plus a STATEMENT of what the host must
// supply.
//
// WHY IT IS A MODULE AND NOT A `printf` IN pack.cppm
//
// Two consumers must produce the same list from the same plan: `mcpp pack`
// writes it beside the artifact, and `mcpp publish` projects it into an xpkg
// descriptor's `[runtime].requirements`. Deriving it twice is how the two
// drift, and a drifted host-requirements list is undetectable — both sides
// look reasonable in isolation.
//
// WHY `discovery` IS A COLUMN
//
// Because the mechanisms are not interchangeable. In the graphics stack, for
// instance, four entry points are four independent loader chains found four
// different ways, so a bare capability name is not actionable:
//
//   GLX        libGLX.so.0's own DT_RPATH points at the vendor directory
//   EGL        a JSON file in __EGL_VENDOR_LIBRARY_DIRS whose `library_path`
//              is an ABSOLUTE path
//   GLESv1/v2  glvnd's dispatch, by SONAME
//   Vulkan     an ICD JSON, independent of the GL stack entirely
//
// Because that JSON holds an absolute path, "copy the directory across" fixes
// one and does nothing for the other. A reader told only the capability cannot
// know that; a reader told the mechanism can.
//
// mcpp does not KNOW any of that, and must not: the value is declared on the
// requirement and carried through. The paragraph above is why the field
// exists, not a table mcpp implements.

export module mcpp.pack.host_requirements;

import std;
import mcpp.manifest;

export namespace mcpp::pack {

struct HostRequirement {
    std::string capability;    // e.g. "opengl.glx.driver"
    std::string discovery;     // declared mechanism; empty = not declared
    bool        required = true;
};

// The file name written at the bundle root. Deliberately not hidden and not an
// extension anyone will double-click: it is meant to be read.
inline constexpr std::string_view kFileName = "HOST-REQUIREMENTS";

// NOTE: mcpp does NOT infer the discovery mechanism from the capability name.
//
// That inference is the exact shape `test_runtime_contract` forbids — a branch
// in mcpp's source on a provider's vocabulary. It is also wrong on its merits:
// which mechanism a capability uses is the PROVIDER's property, it changes
// without mcpp, and a stale guess here would be worse than saying nothing.
// The value travels as declared data on the requirement
// (`[[runtime.requirements]] discovery = "..."`), and an undeclared one is
// reported as `unknown` — which is information, not a gap to fill in.

// THE single derivation. Both `mcpp pack` and `mcpp publish` call this.
//
// A requirement counts when it must be satisfied at RUN time by something
// outside the artifact. Link-phase requirements are consumed during the build
// and say nothing about the target machine.
//
// TAKES THE RESOLVED LIST, NOT A MANIFEST. Almost no application declares
// `capability:opengl.glx.driver` itself — it depends on something that does
// (glfw, an SDL wrapper, a GL runtime), and the resolver stamps each
// requirement with the exact requester. Reading the ROOT manifest's `[runtime]`
// therefore answers "did the author write it down", which is nearly always no,
// while the honest question is "does the resolved graph need it".
//
// Measured: a real imgui project whose `mcpp why runtime` lists
// `capability:opengl.glx.driver [run] <- compat.glfw@3.4 (required)` produced
// an EMPTY list from its own manifest — so `--mode self-contained` packaged it
// happily. The fixture-based test passed because the fixture declared the
// capability at the root, which is the one shape real projects do not have.
std::vector<HostRequirement>
host_requirements_of(std::span<const mcpp::manifest::RuntimeRequirement> requirements,
                     std::span<const std::string> legacyCapabilities = {}) {
    std::vector<HostRequirement> out;
    auto add = [&](std::string capability, std::string discovery, bool required) {
        if (capability.empty()) return;
        if (std::ranges::any_of(out, [&](auto const& r) {
                return r.capability == capability; }))
            return;
        out.push_back({std::move(capability), std::move(discovery), required});
    };
    for (auto const& req : requirements) {
        if (req.phase != "run") continue;
        if (req.kind != "capability") continue;
        add(req.value, req.discovery, req.required);
    }
    // The legacy vector carries the same meaning and is still readable for one
    // compatibility train; a package that has not migrated must not silently
    // produce an empty list. It has no place to declare a mechanism, so those
    // rows say `unknown` — accurately.
    for (auto const& capability : legacyCapabilities)
        add(capability, /*discovery=*/{}, /*required=*/true);
    std::ranges::sort(out, {}, &HostRequirement::capability);
    return out;
}

// Convenience for callers that only have a manifest (e.g. `mcpp emit xpkg`
// describing the package's OWN declarations rather than a resolved graph).
std::vector<HostRequirement>
host_requirements_of(const mcpp::manifest::RuntimeConfig& runtime) {
    return host_requirements_of(runtime.requirements, runtime.capabilities);
}

// Render. One requirement per line, `key=value` fields, so the format can be
// read by a shell one-liner as well as by a program — a manifest nobody can
// grep is a manifest nobody reads.
std::string render(std::span<const HostRequirement> requirements) {
    std::string out =
        "# These must be provided by the TARGET machine. They are not bundled:\n"
        "# a graphics driver's user-space half is version-locked to the running\n"
        "# kernel module, and for the proprietary stacks redistribution is not\n"
        "# permitted. `discovery` is how the loader finds each one -- they are\n"
        "# independent mechanisms, so satisfying one does not satisfy another.\n";
    for (auto const& req : requirements) {
        out += std::format("capability={} discovery={}", req.capability,
                           req.discovery.empty() ? "unknown" : req.discovery);
        if (!req.required) out += " required=false";
        out += '\n';
    }
    return out;
}

} // namespace mcpp::pack
