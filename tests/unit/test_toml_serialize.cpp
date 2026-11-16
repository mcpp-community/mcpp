
import std;
import mcpplibs.mtest;
import mcpp.libs.toml;
import mcpp.manifest;

using namespace mcpplibs::mtest;

// `serialize` is what `mcpp publish` writes a normalised manifest with (#690).
// The property it owes is a round trip: the text it produces parses into the
// same tree AND the same set of explicit tables, because the manifest reader
// uses the explicit set to tell a namespace table from a dotted selector.

namespace t = mcpp::libs::toml;

namespace {

bool same(const t::Value& a, const t::Value& b) {
    if (a.kind() != b.kind()) return false;
    switch (a.kind()) {
        case t::Value::Kind::String: return a.as_string() == b.as_string();
        case t::Value::Kind::Int:    return a.as_int() == b.as_int();
        case t::Value::Kind::Bool:   return a.as_bool() == b.as_bool();
        case t::Value::Kind::Null:   return true;
        case t::Value::Kind::Array: {
            auto const& x = a.as_array();
            auto const& y = b.as_array();
            if (x.size() != y.size()) return false;
            for (std::size_t i = 0; i < x.size(); ++i)
                if (!same(x[i], y[i])) return false;
            return true;
        }
        case t::Value::Kind::Table: {
            auto const& x = a.as_table();
            auto const& y = b.as_table();
            if (x.size() != y.size()) return false;
            for (auto const& [k, v] : x) {
                auto it = y.find(k);
                if (it == y.end() || !same(v, it->second)) return false;
            }
            return true;
        }
    }
    return false;
}

void expect_round_trip(std::string_view source, std::string_view label) {
    auto first = t::parse(source);
    if (auto&& failed = expect(fatal(first.has_value())); !failed) failed << label << ": " << first.error().message;
    auto text = t::serialize(*first);
    auto second = t::parse(text);
    if (auto&& failed = expect(fatal(second.has_value())); !failed) failed << label << ": the serialised text does not parse: "
                                    << second.error().message << "\n" << text;
    expect(bool(same(t::Value{first->root()}, t::Value{second->root()})))
        << label << ": the tree changed\n" << text;
    expect(eq(first->explicit_tables(), second->explicit_tables()))
        << label << ": the explicit tables changed\n" << text;

    // The manifest reader sees the same dependencies. A namespace table that
    // came back as a dotted selector, or the reverse, changes the keys here.
    auto m1 = mcpp::manifest::parse_string(source);
    auto m2 = mcpp::manifest::parse_string(text);
    expect(fatal(eq(m1.has_value(), m2.has_value()))) << label;
    if (!m1) return;
    auto keys = [](auto const& map) {
        std::vector<std::string> out;
        for (auto const& [k, v] : map)
            out.push_back(std::format("{}|{}|{}|{}", k, v.version, v.path, v.namespace_));
        return out;
    };
    expect(eq(keys(m1->dependencies), keys(m2->dependencies))) << label << "\n" << text;
    expect(eq(keys(m1->devDependencies), keys(m2->devDependencies))) << label;
    expect(eq(keys(m1->buildDependencies), keys(m2->buildDependencies))) << label;
    expect(eq(m1->package.name, m2->package.name)) << label;
    expect(eq(m1->package.version, m2->package.version)) << label;
}

} // namespace

UTest headers_inline_tables_and_dotted_keys{"TomlSerialize/HeadersInlineTablesAndDottedKeys", [] {
    expect_round_trip(R"(
[package]
name = "demo"
version = "1.2.3"
authors = ["a", "b \"quoted\""]

[build]
cxxflags = ["-DX=1", "-O2"]
include_dirs = ["include"]

[[build.flags]]
glob = "src/*.cpp"
cxxflags = ["-Wall"]

[[build.flags]]
glob = "src/x.cpp"

[dependencies]
mcpplibs.cmdline = "0.0.1"
"probe.util" = { path = "../util", version = "0.3.0" }
plain = "1.0"

[dependencies.mcpp]
libs = { path = "modules/libs" }

[target.'cfg(os = "linux")'.dependencies]
extra = { version = "2.0", features = ["a"] }

[targets.demo]
kind = "bin"
main = "src/main.cpp"
)", "inline fixture");
}};

UTest implicit_container_with_explicit_descendant{"TomlSerialize/ImplicitContainerWithExplicitDescendant", [] {
    // `dependencies` is created by a dotted key at the root and also has an
    // explicit descendant: its own entries must stay dotted keys.
    expect_round_trip(R"(
dependencies.foo = "1.0"

[dependencies.acme]
bar = "2.0"

[package]
name = "p"
version = "0.1.0"
)", "implicit container");
}};

// The reader accepts every TOML 1.0 escape (#867), so a value may hold any
// control character; the writer escapes each one, so the text it produces is
// TOML and reads back as the same value. `\0` is this reader's extension and
// not TOML 1.0, so it is written as `\u0000`.
UTest control_characters_are_escaped{"TomlSerialize/ControlCharactersAreEscaped", [] {
    expect_round_trip(R"(
[package]
name = "p"
version = "0.1.0"
description = "b\b f\f t\t n\n r\r e\u001B d\u007F z\u0000 nul\0 ué"
)", "control characters");

    std::string all;
    for (int c = 0; c < 0x20; ++c) all.push_back(static_cast<char>(c));
    all.push_back('\x7F');
    const auto text = t::escape_string(all);
    for (char c : text) {
        const auto u = static_cast<unsigned char>(c);
        expect(!(u < 0x20 || u == 0x7F)) << "an unescaped control character in " << text;
    }
    expect(eq(text.find("\\0"), std::string::npos)) << "\\0 is not TOML 1.0: " << text;
    auto back = t::parse("v = " + text + "\n");
    if (auto&& failed = expect(fatal(back.has_value())); !failed) failed << back.error().message << "\n" << text;
    expect(eq(back->root().at("v").as_string(), all));
}};

UTest repository_manifests_round_trip{"TomlSerialize/RepositoryManifestsRoundTrip", [] {
    // Every manifest in the repository's examples, and the project's own. The
    // repository is located from this file's own path rather than from the
    // working directory, and the count is asserted, so a run that found no
    // file cannot pass as one that round-tripped them all.
    auto here = std::filesystem::path(std::source_location::current().file_name());
    auto repo = here.parent_path().parent_path().parent_path();
    expect(fatal(std::filesystem::exists(repo / "mcpp.toml"))) << repo.string();
    std::vector<std::filesystem::path> files{ repo / "mcpp.toml" };
    for (auto const& e : std::filesystem::recursive_directory_iterator(repo / "examples"))
        if (e.is_regular_file() && e.path().filename() == "mcpp.toml")
            files.push_back(e.path());
    expect(ge(files.size(), 10u));
    for (auto const& f : files) {
        std::ifstream is(f, std::ios::binary);
        std::stringstream ss;
        ss << is.rdbuf();
        expect_round_trip(ss.str(), f.string());
    }
}};
