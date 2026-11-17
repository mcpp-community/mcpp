// The source seam's one entry: a private module fragment as a unit of its own.
// The text in, the two units out -- and every fragment that is left as written,
// with the line that says why.

import std;
import mcpplibs.mtest;
import mcpp.buildopt.source;
import mcpp.buildopt.registry;

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
