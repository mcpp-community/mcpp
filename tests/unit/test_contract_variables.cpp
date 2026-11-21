import std;
import mcpplibs.mtest;
import mcpp.build.build_program;

using namespace mcpplibs::mtest;

// CROSS-LAYER: the variables a build program's contract publishes for one channel, judged by
// variable (design 2026-11-21, build programs' inputs §3.7). That a typed reader refuses, or
// answers and warns, is stated by the e2e suite (tests/e2e/build-program/inputs.cpp).

namespace b = mcpp::build;
using Env = std::vector<std::pair<std::string, std::string>>;

namespace {

std::optional<std::string> value(const Env& e, std::string_view var) {
    for (auto const& [k, v] : e)
        if (k == var) return v;
    return std::nullopt;
}

bool each_once(const Env& e) {
    std::set<std::string> seen;
    for (auto const& [k, v] : e)
        if (!seen.insert(k).second) return false;
    return true;
}

b::ContractAnswer dep(std::string spelling, std::string dir) {
    std::string key;
    for (char c : spelling)
        key += std::isalnum(static_cast<unsigned char>(c)) ? static_cast<char>(std::toupper(c)) : '_';
    return {"MCPP_DEP_" + key + "_DIR", dir, spelling, dir, "MCPP_DEP_AMBIGUOUS_" + key};
}

}  // namespace

UTest a_variable_one_owner_answers_is_published{"ContractVariables/OneOwnerIsPublished", [] {
    Env e;
    b::publish_by_variable(e, {dep("zlib", "/z"), dep("mcpplibs.zlib", "/z")});
    expect(eq(value(e, "MCPP_DEP_ZLIB_DIR").value_or(""), std::string("/z")));
    expect(eq(value(e, "MCPP_DEP_MCPPLIBS_ZLIB_DIR").value_or(""), std::string("/z")));
    expect(!value(e, "MCPP_DEP_AMBIGUOUS_ZLIB").has_value());
}};

UTest spellings_no_variable_tells_apart_name_no_usable_spelling{"ContractVariables/IndistinguishableSpellings", [] {
    // `foo-bar` and `foo_bar`, qualified alike: every spelling is one variable of two owners.
    Env e;
    b::publish_by_variable(e, {dep("foo-bar", "/a"), dep("mcpplibs.foo-bar", "/a"),
                               dep("foo_bar", "/b"), dep("mcpplibs.foo_bar", "/b")});
    expect(eq(value(e, "MCPP_DEP_AMBIGUOUS_FOO_BAR").value_or("unset"), std::string()));
    expect(eq(value(e, "MCPP_DEP_AMBIGUOUS_MCPPLIBS_FOO_BAR").value_or("unset"), std::string()));
    // COMPAT(ambiguous-contract-name): the first answer, as before the rule.
    expect(eq(value(e, "MCPP_DEP_FOO_BAR_DIR").value_or(""), std::string("/a")));
    expect(each_once(e));
}};

UTest an_ambiguous_variable_names_the_spellings_that_work{"ContractVariables/AmbiguousNamesUsableSpellings", [] {
    // Two namespaces: the bare spellings collide, the qualified ones do not.
    Env e;
    b::publish_by_variable(e, {dep("foo-bar", "/a"), dep("x.foo-bar", "/a"),
                               dep("foo_bar", "/b"), dep("y.foo_bar", "/b")});
    expect(eq(value(e, "MCPP_DEP_AMBIGUOUS_FOO_BAR").value_or(""), std::string("x.foo-bar,y.foo_bar")));
    expect(eq(value(e, "MCPP_DEP_X_FOO_BAR_DIR").value_or(""), std::string("/a")));
    expect(eq(value(e, "MCPP_DEP_Y_FOO_BAR_DIR").value_or(""), std::string("/b")));
    expect(each_once(e));
}};

UTest two_channels_of_one_key_state_it_once{"ContractVariables/TwoChannelsOfOneKeyStateItOnce", [] {
    // A directory and a link form under one key: one ambiguity variable, each spelling once.
    Env e;
    auto a = dep("foo-bar", "/a"), b2 = dep("foo_bar", "/b");
    auto la = a, lb = b2;
    la.var = "MCPP_DEP_FOO_BAR_LINKAGE"; la.value = "static";
    lb.var = "MCPP_DEP_FOO_BAR_LINKAGE"; lb.value = "static";
    b::publish_by_variable(e, {a, b2, la, lb, dep("x.foo-bar", "/a")});
    expect(eq(value(e, "MCPP_DEP_AMBIGUOUS_FOO_BAR").value_or(""), std::string("x.foo-bar")));
    expect(each_once(e)) << "the ambiguity variable is stated once";
}};

UTest a_bare_payload_two_namespaces_declare_is_ambiguous{"ContractVariables/BarePayloadOfTwoNamespaces", [] {
    Env e;
    const std::vector<b::ContractAnswer> answers{
        {b::xpkg_env_var("xim", "cuda"), "/xim/cuda", "xpkg_dir(\"xim\", \"cuda\")", "xim:cuda", b::xpkg_ambiguous_var("xim", "cuda")},
        {b::xpkg_env_var("", "cuda"), "/xim/cuda", "xpkg_dir(\"cuda\")", "xim:cuda", b::xpkg_ambiguous_var("", "cuda")},
        {b::xpkg_env_var("nv", "cuda"), "/nv/cuda", "xpkg_dir(\"nv\", \"cuda\")", "nv:cuda", b::xpkg_ambiguous_var("nv", "cuda")},
        {b::xpkg_env_var("", "cuda"), "/nv/cuda", "xpkg_dir(\"cuda\")", "nv:cuda", b::xpkg_ambiguous_var("", "cuda")},
    };
    b::publish_by_variable(e, answers);
    expect(eq(value(e, "MCPP_XPKG_AMBIGUOUS_CUDA").value_or(""),
              std::string("xpkg_dir(\"xim\", \"cuda\"),xpkg_dir(\"nv\", \"cuda\")")));
    expect(eq(value(e, "MCPP_XPKG_XIM_CUDA_DIR").value_or(""), std::string("/xim/cuda")));
    expect(eq(value(e, "MCPP_XPKG_CUDA_DIR").value_or(""), std::string("/xim/cuda"))) << "the first declared, deprecated";
    expect(each_once(e));
}};
