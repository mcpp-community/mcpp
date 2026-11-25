// The source seam's one entry: a private module fragment as a unit of its own.
// The text in, the two units out -- and every fragment that is left as written,
// with the line that says why. The corpus below is the verdict for each kind of
// text before the fragment: the directives, the conditions, the lexical forms
// a reader by lines gets wrong, and `conservative`.

import std;
import mcpplibs.mtest;
import mcpp.buildopt.source;
import mcpp.buildopt.registry;
import mcpp.manifest;

using namespace mcpplibs::mtest;
namespace source = mcpp::buildopt::source;

namespace {
const std::string kFile = "/p/src/c.cppm";
bool has(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }
} // namespace

UTest no_fragment{"Source/ATextWithoutAFragmentHasNoUnits", [] {
    expect(!source::private_fragment("export module c;\nexport int f();\n", kFile).has_value());
    // The words in a comment or a string are not the declaration.
    expect(!source::private_fragment("export module c;\n// module :private;\nconst char* s = \"module :private;\";\n", kFile).has_value());
    expect(!source::private_fragment("export module c;\n/* module :private;\n*/\nexport int f();\n", kFile).has_value());
}};

UTest two_units{"Source/AFragmentIsAnImplementationUnitAtItsOwnLines", [] {
    const auto r = source::private_fragment(
        "module;\n#include <cstdio>\nexport module a.b;\nexport int f();\nmodule : private ;\nint f() { return 1; }\n", kFile);
    expect(fatal(r.has_value() && r->has_value()));
    const auto& u = **r;
    expect(eq(u.moduleName, std::string("a.b")));
    expect(eq(u.interfaceUnit, std::string("#line 1 \"/p/src/c.cppm\"\nmodule;\n#include <cstdio>\nexport module a.b;\nexport int f();\n")));
    // The global module fragment as written, the module, then the fragment at line 6.
    expect(eq(u.implementation, std::string("#line 1 \"/p/src/c.cppm\"\nmodule;\n#include <cstdio>\nmodule a.b;\n"
                                            "#line 6 \"/p/src/c.cppm\"\nint f() { return 1; }\n")));
}};

UTest spelling_is_quoted{"Source/TheSpellingIsQuotedForTheDirective", [] {
    const auto r = source::private_fragment("export module c;\nmodule :private;\n", "C:\\p\\c.cppm");
    expect(fatal(r.has_value() && r->has_value()));
    expect(has((**r).interfaceUnit, "#line 1 \"C:\\\\p\\\\c.cppm\"\n"));
}};

UTest left_as_written{"Source/WhatIsLocalToTheInterfaceLeavesTheFragmentAsWritten", [] {
    auto why = [](std::string_view text) {
        const auto r = source::private_fragment(text, kFile);
        return r && !*r ? r->error() : std::string("(split)");
    };
    expect(has(why("export module c;\n#define N 1\nmodule :private;\n"), "line 2: a preprocessor directive"));
    expect(has(why("export module c;\nnamespace {\nint h;\n}\nmodule :private;\n"), "line 2: an unnamed namespace"));
    expect(has(why("export module c;\nnamespace\n{\nint h;\n}\nmodule :private;\n"), "an unnamed namespace"));
    expect(has(why("export module c;\nstatic int h = 1;\nmodule :private;\n"), "line 2: `static` at namespace scope"));
    expect(has(why("export module c;\nnamespace n {\nstatic int h() { return 1; }\n}\nmodule :private;\n"), "`static` at namespace scope"));
    expect(has(why("export module c;\nusing namespace std;\nmodule :private;\n"), "a using-directive"));
    expect(has(why("export module c:part;\nmodule :private;\n"), "a partition"));
    expect(has(why("int x;\nmodule :private;\n"), "no `export module` declaration"));
}};

UTest members_are_not_namespace_scope{"Source/AStaticMemberAndANamedNamespaceAreMovable", [] {
    const auto r = source::private_fragment(
        "export module c;\nexport namespace n {\nstruct S {\n    static int make();\n    static int k;\n};\nusing T = S;\n}\n"
        "module :private;\nint n::S::make() { return 1; }\n", kFile);
    expect(fatal(r.has_value()));
    expect(r->has_value()) << (r->has_value() ? std::string{} : r->error());
}};

namespace {
// `split` or the reason the fragment stays as written.
std::string verdict(std::string_view text, bool conservative = false) {
    const auto r = source::private_fragment(text, kFile, conservative);
    if (!r) return "no-fragment";
    return *r ? std::string("split") : r->error();
}
// The reason without its line number.
std::string kind_of(std::string_view v) {
    if (v.starts_with("line ")) {
        const auto colon = v.find(": ");
        if (colon != std::string_view::npos) return std::string(v.substr(colon + 2));
    }
    return std::string(v);
}
const std::string kHead = "export module c;\n";
const std::string kTail = "module :private;\nint f() { return 1; }\n";
std::string with(std::string_view purview, std::string_view head = "export module c;\n") {
    return std::string(head) + std::string(purview) + kTail;
}
struct Case {
    const char* name;
    std::string text;
    const char* want;       // "split", "no-fragment" or a part of the reason
    const char* wantConservative = nullptr;   // when `conservative` differs
};
} // namespace

// THE DIRECTIVES. A directive that changes what the fragment means refuses: the
// macro or the pragma state is the interface unit's, and does not reach the
// implementation unit. A condition only decides which declarations exist.
UTest corpus{"Source/TheVerdictOfEachKindOfTextBeforeTheFragment", [] {
    const std::vector<Case> cases{
        {"define", with("#define N 1\n"), "a preprocessor directive"},
        {"undef", with("#undef N\n"), "a preprocessor directive"},
        {"include", with("#include <cstdio>\n"), "a preprocessor directive"},
        {"pragma", with("#pragma once\n"), "a preprocessor directive"},
        {"line", with("#line 5\n"), "a preprocessor directive"},
        {"unknown", with("#ident \"x\"\n"), "a preprocessor directive"},
        {"marker", with("# 12 \"f.h\"\n"), "a preprocessor directive"},
        {"spaces", with("  #  define N 1\n"), "a preprocessor directive"},
        {"_Pragma", with("_Pragma(\"once\") int x;\n"), "a pragma"},
        {"__pragma", with("__pragma(warning(disable: 1)) int x;\n"), "a pragma"},
        {"if", with("#if A\nexport int g();\n#endif\n"), "split", "a preprocessor directive"},
        {"ifdef-else", with("#ifdef A\nexport int g();\n#else\nexport int g(int);\n#endif\n"), "split", "a preprocessor directive"},
        {"ifndef-elif", with("#ifndef A\nexport int g();\n#elif B\nexport int h();\n#endif\n"), "split", "a preprocessor directive"},
        {"elifdef", with("#if A\n#elifdef B\nexport int g();\n#endif\n"), "split", "a preprocessor directive"},
        {"error", with("#if 0\n#error never\n#endif\n"), "split", "a preprocessor directive"},
        {"warning-with-quote", with("#warning don't\n"), "split", "a preprocessor directive"},
        {"null-directive", with("#\n"), "split", "a preprocessor directive"},
        {"define-in-condition", with("#if A\n#define N 1\n#endif\n"), "a preprocessor directive"},
        {"condition-spans-fragment", kHead + "#if A\nexport int g();\n" + kTail + "#endif\n", "open at the fragment",
         "a preprocessor directive"},
        {"declaration-in-condition", "#if A\nexport module c;\n#endif\n" + kTail, "open at the module declaration",
         "a preprocessor directive"},
        {"balanced-before-declaration", "module;\n#if A\n#include <x>\n#endif\nexport module c;\nexport int g();\n" + kTail, "split"},
        {"stray-endif", with("#endif\n"), "open at the fragment", "a preprocessor directive"},
        {"global-fragment-define", "module;\n#define N 1\n#include <cstdio>\nexport module c;\nexport int g();\n" + kTail, "split"},
        // The scope the fragment cannot see.
        {"unnamed-namespace", with("namespace {\nint h;\n}\n"), "an unnamed namespace"},
        {"static-variable", with("static int h = 1;\n"), "`static` at namespace scope"},
        {"static-in-named", with("namespace n {\nstatic int h() { return 1; }\n}\n"), "`static` at namespace scope"},
        {"using-directive", with("using namespace std;\n"), "a using-directive"},
        {"partition", "export module c:p;\n" + kTail, "a partition"},
        {"no-declaration", "int x;\n" + kTail, "no `export module` declaration"},
        {"no-fragment", "export module c;\nexport int g();\n", "no-fragment"},
        // What a reader by lines gets wrong.
        {"digit-separator-hides-static", with("constexpr int k = 1'000; static int h = 2;\n"), "`static` at namespace scope"},
        {"digit-separator-hex", with("constexpr unsigned k = 0xFF'FFu; static int h = 2;\n"), "`static` at namespace scope"},
        {"digit-separator-binary", with("constexpr int k = 0b1'0; constexpr int j = 1'0'0;\n"), "split"},
        {"exponent-sign", with("constexpr double d = 1.5e+3; static int h;\n"), "`static` at namespace scope"},
        {"char-literal", with("constexpr char c = 'a'; constexpr char d = '\\'';\n"), "split"},
        {"u8-char", with("constexpr char8_t c = u8'a';\n"), "split"},
        {"string-with-brace", with("const char* s = \"{ static int\";\n"), "split"},
        {"raw-string-with-namespace", with("const char* s = R\"x(\nnamespace {\nstatic int h;\n)x\";\n"), "split"},
        {"raw-string-with-fragment-words", "export module c;\nconst char* s = R\"(\nmodule :private;\n)\";\n", "no-fragment"},
        {"raw-string-prefix", with("const char* s = u8R\"(namespace {)\";\n"), "split"},
        {"raw-string-delimiter", with("const char* s = R\"--(\")(namespace {)--\";\n"), "split"},
        {"not-a-raw-string", with("int xR = 1; const char* s = \"static\";\n"), "split"},
        {"extern-c-static", with("extern \"C\" {\nstatic int h();\n}\n"), "`static` at namespace scope"},
        {"extern-cxx-static", with("extern \"C++\" {\nstatic int h;\n}\n"), "`static` at namespace scope"},
        {"extern-c-plain", with("extern \"C\" {\nint g();\n}\n"), "split"},
        {"extern-c-in-function", with("void g() {\nextern \"C\" int h();\nstatic int n = 0;\n}\n"), "split"},
        {"continuation", with("constexpr int k = 1 + \\\n2;\n"), "a line continuation"},
        {"continuation-crlf", with("constexpr int k = 1 + \\\r\n2;\n"), "a line continuation"},
        {"continued-comment", with("// a comment \\\nstatic int h;\n"), "a line continuation"},
        {"continuation-in-global-fragment", "module;\n#define X \\\n  1\nexport module c;\nexport int g();\n" + kTail, "split"},
        {"comment-with-quote", with("// don't \"quote\nexport int g();\n"), "split"},
        {"block-comment-with-fragment", with("/* module :private;\nstatic int h; */\n"), "split"},
    };
    for (auto const& c : cases) {
        const auto got = verdict(c.text);
        expect(has(got, c.want)) << c.name << ": " << got;
        const auto want = c.wantConservative ? c.wantConservative : c.want;
        const auto conservative = verdict(c.text, true);
        expect(has(conservative, want)) << c.name << " (conservative): " << conservative;
    }
}};

// The two units hold the text between them: the interface unit the lines before
// the fragment, the implementation unit the lines after it, at the lines they
// had (its `#line` says so).
UTest units_hold_the_text{"Source/TheTwoUnitsHoldEveryLineOfTheText", [] {
    const std::vector<std::string> texts{
        "module;\n#include <cstdio>\nexport module a.b;\nexport int f();\nmodule : private ;\nint f() { return 1; }\n",
        "export module c;\n#if A\nexport int g();\n#endif\nmodule :private;\nint g() { return 2; }\nint h() { return 3; }",
        "export module c;\nmodule :private;\n",
        "export module c;\r\nexport int g();\r\nmodule :private;\r\nint g() { return 1; }\r\n",
    };
    auto lines_of = [](std::string_view u) {
        std::vector<std::string> out;
        for (std::size_t at = 0; at <= u.size();) {
            const auto end = u.find('\n', at);
            auto l = std::string(u.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at));
            if (!l.starts_with("#line ")) out.push_back(std::move(l));
            if (end == std::string_view::npos) break;
            at = end + 1;
        }
        return out;
    };
    for (auto const& text : texts) {
        const auto r = source::private_fragment(text, kFile);
        expect(fatal(r.has_value() && r->has_value())) << text;
        const auto interfaceLines = lines_of((**r).interfaceUnit);
        const auto implementationLines = lines_of((**r).implementation);
        const auto original = lines_of(text);
        expect(!implementationLines.empty()) << text;
        const auto fragmentAt = std::ranges::find_if(original, [](const std::string& l) {
            std::string c;
            for (char ch : l) if (ch != ' ' && ch != '\t' && ch != '\r') c += ch;
            return c == "module:private;";
        });
        expect(fragmentAt != original.end()) << text;
        // The interface unit holds the lines before the fragment.
        expect(std::ranges::equal(std::span(interfaceLines).first(static_cast<std::size_t>(fragmentAt - original.begin())),
                                  std::span(original).first(static_cast<std::size_t>(fragmentAt - original.begin())))) << text;
        // The implementation unit carries the lines after the fragment as they were.
        const auto after = std::vector<std::string>(fragmentAt + 1, original.end());
        expect(implementationLines.size() >= after.size()) << text;
        expect(std::ranges::equal(std::span(implementationLines).last(after.size()), after)) << text;
    }
}};

// A line that is only a comment or a digit separator in a literal changes no
// verdict: the reason is the same text, a line further.
UTest lexical_noise_changes_nothing{"Source/ACommentOrADigitSeparatorChangesNoVerdict", [] {
    const std::vector<std::string> purviews{
        "", "static int h = 1;\n", "namespace {\nint h;\n}\n", "#define N 1\n", "#if A\nexport int g();\n#endif\n",
        "using namespace std;\n", "extern \"C\" {\nstatic int h;\n}\n",
    };
    for (auto const& purview : purviews) {
        const auto plain = verdict(with(purview));
        for (std::string_view noise : {"// don't \"quote \\'\n", "/* 1'000 \" */\n", "constexpr int n_ = 1'000'000;\n",
                                       "const char* r_ = R\"(\" ' )\";\n"}) {
            const auto got = verdict(with(std::string(noise) + purview));
            expect(eq(kind_of(plain), kind_of(got))) << purview << " + " << noise << ": " << got;
        }
    }
}};

// Whatever the text, the answer is a verdict, never a crash; a text of
// fragments of the language is read in the hundreds.
UTest any_text_has_a_verdict{"Source/AnyTextHasAVerdict", [] {
    const std::vector<std::string_view> pieces{
        "export module c;\n", "module :private;\n", "module;\n", "#if A\n", "#endif\n", "#define N 1\n", "namespace {\n", "}\n",
        "static int h;\n", "'", "\"", "R\"x(", ")x\"", "\\\n", "/*", "*/", "//", "1'0", "extern \"C\" {\n", "using namespace s;\n",
        "\r\n", "int f() { return 1; }\n", "export int g();\n", " ", "\t", "\n", "{", "}", ";",
    };
    std::mt19937 gen(20261126);
    for (int n = 0; n < 400; ++n) {
        std::string text;
        for (int k = std::uniform_int_distribution<int>(1, 14)(gen); k > 0; --k)
            text += pieces[std::uniform_int_distribution<std::size_t>(0, pieces.size() - 1)(gen)];
        for (bool conservative : {false, true}) {
            const auto r = source::private_fragment(text, kFile, conservative);
            if (r && *r) {
                // A split unit scans as the module it was: it still names it.
                expect(has((**r).implementation, "module c;")) << text;
                expect(has((**r).interfaceUnit, "export module c;")) << text;
            }
        }
    }
}};

UTest policy_is_the_entrys_switch{"Source/TheFragmentPolicyIsTheEntrysSwitch", [] {
    mcpp::manifest::Manifest m;
    std::string invalid;
    auto policy = [&] { return source::private_fragment_policy(m, [&](std::string_view v) { invalid = std::string(v); }); };
    expect(policy() == source::Fragment::Unit);
    m.buildConfig.privateFragment = "unit";
    expect(policy() == source::Fragment::Unit);
    m.buildConfig.privateFragment = "conservative";
    expect(policy() == source::Fragment::Conservative);
    m.buildConfig.privateFragment = "as-written";
    expect(policy() == source::Fragment::AsWritten);
    expect(invalid.empty());
    m.buildConfig.privateFragment = "split";
    expect(policy() == source::Fragment::Unit);
    expect(eq(invalid, std::string("split")));
}};

UTest register_states_every_entry{"Registry/EveryEntryStatesItsDefaultAndWhatStatesOtherwise", [] {
    namespace registry = mcpp::buildopt::registry;
    std::set<std::string_view> ids;
    for (auto const& e : registry::kEntries) {
        expect(ids.insert(e.id).second) << e.id;
        expect(!e.what.empty()) << e.id;
        // An exact entry has nothing to choose; every other states its key, its
        // environment name and its values.
        if (e.level == registry::Level::Exact) expect(e.key.empty() && e.byDefault) << e.id;
        else expect(!e.key.empty() && !e.env.empty() && !e.values.empty()) << e.id;
        // A shape is off until it has run on every host.
        if (e.level == registry::Level::Shape) expect(!e.byDefault) << e.id;
        expect(has(registry::table(), std::format("| `{}` |", e.id))) << e.id;
    }
    expect(registry::find(registry::kPrivateFragment) != nullptr);
    expect(registry::find("no-such-entry") == nullptr);
}};

// The README lists the register: an entry added to one is added to the other.
UTest readme_lists_the_register{"Registry/TheReadmeListsEveryEntryWithItsDefault", [] {
    namespace registry = mcpp::buildopt::registry;
    const auto readme = std::filesystem::path(std::source_location::current().file_name()).parent_path().parent_path() / "README.md";
    std::ifstream in(readme, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(in), {}};
    if (text.empty()) skip("the package's README is not beside its tests here");
    for (auto const& e : registry::kEntries) {
        const auto row = std::format("| `{}` | {} | {} | {} |", e.id, registry::to_string(e.seam),
                                     registry::to_string(e.level), e.byDefault ? "on" : "off");
        expect(has(text, row)) << row;
        if (!e.env.empty()) expect(has(text, e.env)) << e.env;
    }
    expect(has(text, registry::kStandardEnv));
}};
