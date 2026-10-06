// mcpp.build.pe_exports — export intent precedes candidate discovery, whether
// the MSVC-ABI linker inputs are COFF, LLVM bitcode, or a mixture of both.
export module mcpp.build.pe_exports;

import std;
import mcpp.build.coff_exports;
import mcpp.modgraph.glob;
import mcpp.platform.fs;
import mcpp.platform.process;
import mcpp.toolchain.triple;

export namespace mcpp::build::pe {

struct LLVMTools {
    std::filesystem::path compiler;
    std::filesystem::path nm;
    std::string target;
};

bool is_bitcode(std::span<const std::byte> bytes);
bool ir_declares_exports(std::string_view ir);
std::expected<std::vector<coff::Export>, std::string>
read_nm_exports(std::string_view text, bool i386);

// What the inputs of one MSVC-ABI DLL say about its exported surface.
//
// An input that declares exports itself (`__declspec(dllexport)`, `/EXPORT:`
// directives, linker-option metadata) decides the surface for the whole DLL:
// the linker reads those declarations whatever the `.def` says, so `declaredBy`
// names the first such input and `candidates` is empty. No candidate reader is
// entered on that path; in particular, annotated COFF does not require LLVM
// tools merely because another input happens to be LTO. Otherwise
// `candidates` holds every exportable definition the inputs contain.
struct Discovery {
    std::optional<std::filesystem::path> declaredBy;
    std::vector<coff::Export>            candidates;
};
std::expected<Discovery, std::string>
discover_exports(std::span<const std::filesystem::path> objects, const LLVMTools& tools);

// `[targets.<n>] exports` on the MSVC ABI: the candidates whose name matches
// one of `patterns`, with the glob semantics of an ELF version script (`*`,
// `?`, `[...]`, `[!...]`). A pattern matches the linker-level name: undecorated
// on i386, as coff::export_name spells it, and MSVC-mangled for C++.
bool symbol_matches(std::string_view pattern, std::string_view name);
std::vector<coff::Export> narrow(std::vector<coff::Export> candidates,
                                 std::span<const std::string> patterns);

} // namespace mcpp::build::pe

namespace mcpp::build::pe {
namespace {

// LLVM's printer quotes names and strings and uses hexadecimal escapes inside
// them. Keep those tokens whole: neither a name nor string data is DLL storage
// intent. Semicolon comments are also outside the token stream.
std::vector<std::string_view> tokens(std::string_view line) {
    std::vector<std::string_view> out;
    for (std::size_t i = 0; i < line.size();) {
        if (std::isspace(static_cast<unsigned char>(line[i]))) { ++i; continue; }
        if (line[i] == ';') break;
        auto begin = i++;
        if (line[begin] == '"') {
            while (i < line.size()) {
                if (line[i] == '\\') { i = std::min(i + 3, line.size()); continue; }
                if (line[i++] == '"') break;
            }
        } else if (std::string_view("{}()[]=,").find(line[begin]) == std::string_view::npos) {
            while (i < line.size()
                && !std::isspace(static_cast<unsigned char>(line[i]))
                && std::string_view("{}()[]=,;\"").find(line[i]) == std::string_view::npos)
                ++i;
        }
        out.push_back(line.substr(begin, i - begin));
    }
    return out;
}

std::string unquote(std::string_view token) {
    std::string out;
    if (token.size() < 2 || token.front() != '"' || token.back() != '"') return out;
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 1; i + 1 < token.size(); ++i) {
        if (token[i] == '\\' && i + 3 < token.size()) {
            int hi = hex(token[i + 1]), lo = hex(token[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(token[i]);
    }
    return out;
}

bool export_directive(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto spelling : {"/export:", "-export:"}) {
        auto pos = text.find(spelling);
        while (pos != std::string::npos) {
            if (pos == 0 || text[pos - 1] == '"'
                || std::isspace(static_cast<unsigned char>(text[pos - 1]))) return true;
            pos = text.find(spelling, pos + 1);
        }
    }
    return false;
}

std::expected<std::vector<std::byte>, std::string>
read_object(const std::filesystem::path& path) {
    std::ifstream in(mcpp::platform::fs::extended_length(path), std::ios::binary);
    if (!in) return std::unexpected("cannot read object");
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) return std::unexpected("cannot finish reading object");
    auto bytes = std::as_bytes(std::span(data));
    return std::vector<std::byte>(bytes.begin(), bytes.end());
}

std::expected<std::string, std::string>
run_tool(const std::filesystem::path& tool, std::vector<std::string> args) {
    if (tool.empty())
        return std::unexpected("LLVM bitcode requires tools from the selected LLVM installation");
    auto name = mcpp::modgraph::try_narrow(tool);
    if (!name) return std::unexpected("the selected LLVM tool has no UTF-8 spelling");
    args.insert(args.begin(), *name);
    auto result = mcpp::platform::process::capture_exec(args);
    if (result.exit_code != 0)
        return std::unexpected(std::format("{} exited with status {}:\n{}",
            *name, result.exit_code, result.output));
    return std::move(result.output);
}

} // namespace

bool is_bitcode(std::span<const std::byte> bytes) {
    if (bytes.size() < 4) return false;
    const std::array raw{std::byte{0x42}, std::byte{0x43}, std::byte{0xc0}, std::byte{0xde}};
    const std::array wrapped{std::byte{0xde}, std::byte{0xc0}, std::byte{0x17}, std::byte{0x0b}};
    return std::ranges::equal(bytes.first(4), raw)
        || std::ranges::equal(bytes.first(4), wrapped);
}

bool ir_declares_exports(std::string_view ir) {
    std::map<std::string_view, std::vector<std::string_view>> metadata;
    for (auto line : ir | std::views::split('\n')) {
        auto text = std::string_view(line);
        auto begin = text.find_first_not_of(" \t\r");
        if (begin == std::string_view::npos) continue;
        text.remove_prefix(begin);
        // Bodies, debug comments and attribute lists cannot carry export
        // intent. Do not tokenize every instruction of a large LTO input.
        if (text.front() != '@' && text.front() != '!'
            && !text.starts_with("define ") && !text.starts_with("declare ")
            && !text.starts_with("module asm ")) continue;
        auto words = tokens(text);
        if (words.empty()) continue;
        if (words.front() == "define" || words.front() == "declare"
            || words.front().starts_with('@')) {
            // The declaration header ends at the argument list / initializer.
            // A local identifier or data literal named dllexport is not a flag.
            for (auto word : words) {
                if (word == "dllexport") return true;
                if (word == "(" || word == "global" || word == "constant"
                    || word == "alias" || word == "ifunc") break;
            }
        }
        if (words.front() == "module" && words.size() >= 3 && words[1] == "asm"
            && export_directive(unquote(words.back()))) return true;
        if (words.size() >= 3 && words.front().starts_with('!') && words[1] == "=")
            metadata.emplace(words.front(), std::move(words));
    }

    // A pragma's linker options survive LTO as metadata. Follow only this root;
    // debug metadata and unrelated string constants are not linker directives.
    std::vector<std::string_view> pending{"!llvm.linker.options"};
    std::set<std::string_view> visited;
    while (!pending.empty()) {
        auto key = pending.back();
        pending.pop_back();
        if (!visited.insert(key).second) continue;
        auto it = metadata.find(key);
        if (it == metadata.end()) continue;
        for (std::size_t i = 2; i < it->second.size(); ++i) {
            auto word = it->second[i];
            if (word.starts_with('"') && export_directive(unquote(word))) return true;
            if (word.size() > 1 && word.front() == '!'
                && std::isdigit(static_cast<unsigned char>(word[1]))) pending.push_back(word);
        }
    }
    return false;
}

std::expected<std::vector<coff::Export>, std::string>
read_nm_exports(std::string_view text, bool i386) {
    std::vector<coff::Export> out;
    for (auto line : text | std::views::split('\n')) {
        std::istringstream in{std::string(std::string_view(line))};
        std::string name, type, value;
        if (!(in >> name)) continue;
        if (!(in >> type >> value) || type.size() != 1)
            return std::unexpected("unexpected llvm-nm POSIX output: " + std::string(std::string_view(line)));
        // COFF COMDAT definitions are external symbols too: weak definitions
        // must not disappear merely because the compiler used LTO.
        const bool code = type == "T" || type == "W";
        const bool data = type == "D" || type == "B" || type == "R" || type == "V";
        if (!code && !data) continue;
        auto exported = coff::export_name(name, i386);
        if (exported) out.push_back({std::move(*exported), data});
    }
    return out;
}

namespace {

// The tool runs of one call, `jobs` at a time and in input order. Each
// inspects one object, and the objects of a large LTO DLL number in the
// hundreds, so running them one after another dominated the `.def` edge.
template <class Fn>
void for_each_concurrently(std::size_t count, Fn&& fn) {
    const std::size_t jobs = std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1, 8);
    std::atomic<std::size_t> next{0};
    std::vector<std::jthread> workers;
    for (std::size_t w = 0; w < std::min(jobs, count); ++w)
        workers.emplace_back([&] {
            for (std::size_t i = next++; i < count; i = next++) fn(i);
        });
}

} // namespace

std::expected<Discovery, std::string>
discover_exports(std::span<const std::filesystem::path> objects, const LLVMTools& tools) {
    auto error = [](const std::filesystem::path& obj, std::string message) {
        return std::unexpected(mcpp::modgraph::escaped_spelling(obj) + ": " + message);
    };
    std::vector<bool> bitcode;
    for (auto const& obj : objects) {
        auto bytes = read_object(obj);
        if (!bytes) return error(obj, bytes.error());
        bitcode.push_back(is_bitcode(*bytes));
        if (!bitcode.back() && coff::declares_exports(*bytes))
            return Discovery{.declaredBy = obj, .candidates = {}};
    }
    std::vector<std::string> arguments;
    for (auto const& obj : objects) {
        auto arg = mcpp::modgraph::try_narrow(obj);
        if (!arg) return error(obj, "object has no UTF-8 spelling");
        arguments.push_back(std::move(*arg));
    }
    const auto targetTriple = mcpp::toolchain::triple::parse(tools.target);
    const bool i386 = targetTriple && targetTriple->is_x86_32();
    std::vector<std::size_t> lto;
    for (std::size_t i = 0; i < objects.size(); ++i)
        if (bitcode[i]) lto.push_back(i);
    if (!lto.empty() && tools.target.empty())
        return error(objects[lto.front()], "LLVM bitcode inspection requires the selected target triple");

    // Intent first, for every bitcode input; the first declaring input in
    // input order is the one named.
    std::vector<std::expected<bool, std::string>> declares(lto.size(), false);
    for_each_concurrently(lto.size(), [&](std::size_t k) {
        const auto i = lto[k];
        std::vector<std::string> args{"--driver-mode=g++", "-S", "-emit-llvm", "-x", "ir",
                                      "--target=" + tools.target, arguments[i], "-o", "-"};
        auto ir = run_tool(tools.compiler, std::move(args));
        if (!ir) declares[k] = std::unexpected(ir.error());
        else     declares[k] = ir_declares_exports(*ir);
    });
    for (std::size_t k = 0; k < lto.size(); ++k) {
        if (!declares[k]) return error(objects[lto[k]], declares[k].error());
        if (*declares[k]) return Discovery{.declaredBy = objects[lto[k]], .candidates = {}};
    }

    std::vector<std::expected<std::vector<coff::Export>, std::string>> symbols(
        objects.size(), std::vector<coff::Export>{});
    for_each_concurrently(lto.size(), [&](std::size_t k) {
        const auto i = lto[k];
        auto text = run_tool(tools.nm, {"--quiet", "--format=posix", "--extern-only", "--defined-only",
                                      "--no-demangle", arguments[i]});
        if (!text) symbols[i] = std::unexpected(text.error());
        else       symbols[i] = read_nm_exports(*text, i386);
    });
    Discovery out;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        if (!bitcode[i]) {
            auto bytes = read_object(objects[i]);
            if (!bytes) return error(objects[i], bytes.error());
            symbols[i] = coff::read_exports(*bytes);
        }
        if (!symbols[i]) return error(objects[i], symbols[i].error());
        out.candidates.insert(out.candidates.end(),
                              std::make_move_iterator(symbols[i]->begin()),
                              std::make_move_iterator(symbols[i]->end()));
    }
    return out;
}

bool symbol_matches(std::string_view pattern, std::string_view name) {
    // Iterative glob with single-star backtracking, as fnmatch does it without
    // FNM_PATHNAME: a symbol name has no separator to respect.
    std::size_t p = 0, n = 0, starP = std::string_view::npos, starN = 0;
    auto class_matches = [&](std::size_t& at, char c) -> std::optional<bool> {
        // `at` is on '['. Returns nullopt for an unterminated class, which is
        // then an ordinary character.
        std::size_t i = at + 1;
        bool negate = false;
        if (i < pattern.size() && (pattern[i] == '!' || pattern[i] == '^')) { negate = true; ++i; }
        bool hit = false;
        bool first = true;
        for (; i < pattern.size() && (first || pattern[i] != ']'); ++i, first = false) {
            if (i + 2 < pattern.size() && pattern[i + 1] == '-' && pattern[i + 2] != ']') {
                if (pattern[i] <= c && c <= pattern[i + 2]) hit = true;
                i += 2;
            } else if (pattern[i] == c) {
                hit = true;
            }
        }
        if (i >= pattern.size()) return std::nullopt;
        at = i;   // on ']'
        return hit != negate;
    };
    while (n < name.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            starP = p++;
            starN = n;
            continue;
        }
        if (p < pattern.size()) {
            if (pattern[p] == '?') { ++p; ++n; continue; }
            if (pattern[p] == '[') {
                std::size_t at = p;
                if (auto m = class_matches(at, name[n])) {
                    if (*m) { p = at + 1; ++n; continue; }
                } else if (name[n] == '[') { ++p; ++n; continue; }
            } else if (pattern[p] == name[n]) { ++p; ++n; continue; }
        }
        if (starP == std::string_view::npos) return false;
        p = starP + 1;
        n = ++starN;
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

std::vector<coff::Export> narrow(std::vector<coff::Export> candidates,
                                 std::span<const std::string> patterns) {
    std::erase_if(candidates, [&](const coff::Export& e) {
        return std::ranges::none_of(patterns, [&](const std::string& p) {
            return symbol_matches(p, e.name);
        });
    });
    return candidates;
}

} // namespace mcpp::build::pe
