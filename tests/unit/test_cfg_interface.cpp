import std;
import mcpplibs.mtest;
import mcpp.build.prepare_inputs;

using namespace mcpplibs::mtest;

namespace cfgpred = mcpp::build::cfgpred;

namespace {

// A context whose kernel-abi layer resolved to `impl`, its provider stating
// `interfaces` (`stated`) or no list at all.
cfgpred::Ctx with(std::string impl, std::vector<std::string> interfaces, bool stated = true) {
    auto c = cfgpred::context_for("x86_64-linux-gnu");
    c.layersKnown      = true;
    c.kernelAbi        = std::move(impl);
    c.interfaces       = std::move(interfaces);
    c.interfacesStated = stated;
    return c;
}

bool m(std::string_view predicate, const cfgpred::Ctx& c) {
    return cfgpred::matches(std::string(predicate), c);
}

constexpr std::string_view kFull =
    R"(cfg(all(interface = "openkal.fs", interface = "openkal.process", interface = "openkal.task")))";

} // namespace

// `interface` is the kernel-abi layer's second answer: membership in the
// provider's `[kernel-abi] provides-interfaces`, as `accelerator` is a set.
UTest membership{"CfgInterface/Membership", [] {
    const auto firmware = with("openkal", {"openkal.abort", "openkal.stream", "openkal.exec"});
    expect(bool(m(R"(cfg(interface = "openkal.exec"))", firmware)));
    expect(!(m(R"(cfg(interface = "openkal.fs"))", firmware)));
    expect(!(m(kFull, firmware)));
    expect(bool(m(std::format("cfg(not({}))", kFull.substr(4, kFull.size() - 5)), firmware)));

    const auto hosted = with("openkal", {"openkal.fs", "openkal.process", "openkal.task", "openkal.exec"});
    expect(bool(m(kFull, hosted)));
}};

// A provider that states no list provides every interface (the rule
// `requires-interfaces` is checked by), so a selection written for the full
// set holds for it.
UTest no_list_is_every_interface{"CfgInterface/NoListIsEveryInterface", [] {
    const auto c = with("openkal", {}, false);
    expect(bool(m(kFull, c)));
    expect(bool(m(R"(cfg(interface = "openkal.anything"))", c)));
}};

// A graph without the kernel-abi layer provides no interface, and before the
// layers resolve the key answers nothing: the second pass owns it.
UTest no_layer_no_interface{"CfgInterface/NoLayerNoInterface", [] {
    expect(!(m(R"(cfg(interface = "openkal.fs"))", with("", {}, false))));
    auto early = with("openkal", {"openkal.fs"});
    early.layersKnown = false;
    expect(!(m(R"(cfg(interface = "openkal.fs"))", early)));
    expect(bool(cfgpred::uses_layer(R"(cfg(interface = "openkal.fs"))")));
}};
