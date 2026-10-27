// mcpp.manifest.cfg_selector — the `[target.<selector>]` vocabulary, and the
// order in which matching conditional tables apply (SPEC-004 §3.1.1).
//
// THE ORDER IS THE SELECTOR'S SPECIFICITY (D7 of the 2026-09-28 design, #728).
// Several `[target.<selector>.<section>]` tables can match one resolved target;
// for a scalar or a dependency identity the later one replaces the earlier,
// and list values are appended in the same order. SPEC-004 said "manifest
// order", which a TOML reader cannot observe: the keys of a table carry no
// order, and `mcpp.libs.toml` stores them in a `std::map`, so the tables
// applied in the lexical order of their selector text -- `aarch64-unknown-
// linux-gnu` before `linux`, which then replaced the triple's statement. The
// rule now: a more specific selector applies later, so it wins; lexical order
// breaks a tie.
//
// Specificity is the set of target-triple components a selector fixes:
//
//   a bare triple (`x86_64-unknown-linux-gnu`)   every component; above any cfg
//   `os = "…"`, `linux`, `macos`, `windows`       the OS and its family
//   `family = "…"`, `unix`                        the family
//   `arch = "…"`, `env = "…"`                     that component
//   `all(a, b, …)`                                the union of its terms
//   `any(…)`, `not(…)`, a layer key, `accelerator`   no component
//
// so a triple outranks an OS, which outranks a family, as D7 settled, and
// `cfg(all(os = "linux", arch = "aarch64"))` outranks `cfg(os = "linux")`.
//
// THE VOCABULARY LIVES HERE, ONCE. The build layer's evaluator of the same
// grammar (`mcpp.build.prepare_inputs`, cfgpred) reads these lists; the
// ranking below reads the same ones, so a key added to the vocabulary cannot
// be known to one reader and unknown to the other.

export module mcpp.manifest.cfg_selector;

import std;
import mcpp.manifest.types;

export namespace mcpp::manifest::cfg {

// TRIPLE keys are answerable from the target triple alone. LAYER keys name a
// target-side layer and are answerable only after the graph is resolved (see
// the note on `kCfgLayerKeys` in mcpp.build.prepare_inputs); `accelerator` is
// an input, known before resolution.
// `platform` is answered by the platform selection (SPEC-013 §3), which is
// made from the triple and the configuration before resolution, so it is read
// in the same pass as the triple's own keys.
inline constexpr std::string_view kCfgTripleKeys[] = {
    "arch", "env", "family", "os", "platform",
};
inline constexpr std::string_view kCfgEarlyLayerKeys[] = {
    "accelerator",
};
inline constexpr std::string_view kCfgLayerKeys[] = {
    "c++-abi", "c-abi", "compiler", "compiler-runtime",
    "kernel-abi",
};
inline constexpr std::string_view kCfgBarewords[] = {
    "linux", "macos", "unix", "windows",
};

// The rank of a selector: the number of target-triple components it fixes, or
// `kTripleRank` for a bare triple. Larger is more specific.
inline constexpr int kTripleRank = 5;
int specificity(std::string_view selector);

// Order conditional tables so that a more specific selector applies later,
// with the selector text breaking a tie. Called by the parsers, so every
// reader of `conditionalConfigs` sees the one order.
void order_by_specificity(std::vector<ConditionalConfig>& tables);

} // namespace mcpp::manifest::cfg

// ── implementation ──────────────────────────────────────────────────────────

namespace mcpp::manifest::cfg {

namespace {

constexpr unsigned kArch = 1, kOs = 2, kEnv = 4, kFamily = 8, kPlatform = 16;

unsigned component_of_key(std::string_view key) {
    if (key == "arch")   return kArch;
    if (key == "os")     return kOs | kFamily;
    if (key == "env")    return kEnv;
    if (key == "family") return kFamily;
    // A platform names a class of targets: one component, less specific than
    // an operating system (two) and than a triple.
    if (key == "platform") return kPlatform;
    return 0;   // a layer key, `accelerator`, or an unknown key
}

unsigned component_of_bareword(std::string_view word) {
    if (word == "linux" || word == "macos" || word == "windows") return kOs | kFamily;
    if (word == "unix") return kFamily;
    return 0;
}

// The grammar of `cfg(...)`: all(list) | any(list) | not(expr) | key = "value"
// | bareword, with `-` and `+` as identifier characters (the layer names).
struct Scan {
    std::string_view s;
    std::size_t i = 0;
    void ws() { while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; }
    bool eat(char ch) { ws(); if (i < s.size() && s[i] == ch) { ++i; return true; } return false; }
    std::string_view ident() {
        ws();
        const auto b = i;
        while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i]))
                                || s[i] == '_' || s[i] == '-' || s[i] == '+')) ++i;
        return s.substr(b, i - b);
    }
    void str() {
        ws();
        if (i >= s.size() || s[i] != '"') return;
        ++i;
        while (i < s.size() && s[i] != '"') ++i;
        if (i < s.size()) ++i;
    }
    unsigned expr() {
        const auto id = ident();
        if (id == "all" || id == "any") {
            eat('(');
            unsigned acc = 0;
            ws();
            if (!(i < s.size() && s[i] == ')')) {
                do { acc |= expr(); } while (eat(','));
            }
            eat(')');
            return id == "all" ? acc : 0u;
        }
        if (id == "not") {
            eat('(');
            (void)expr();
            eat(')');
            return 0u;
        }
        ws();
        if (i < s.size() && s[i] == '=') {
            ++i;
            str();
            return component_of_key(id);
        }
        return component_of_bareword(id);
    }
};

} // namespace

int specificity(std::string_view selector) {
    if (selector.starts_with("cfg(") && selector.ends_with(")")) {
        Scan scan{selector.substr(4, selector.size() - 5)};
        return std::popcount(scan.expr());
    }
    if (const auto bare = component_of_bareword(selector); bare != 0)
        return std::popcount(bare);
    return kTripleRank;
}

void order_by_specificity(std::vector<ConditionalConfig>& tables) {
    std::ranges::stable_sort(tables, [](const ConditionalConfig& a, const ConditionalConfig& b) {
        const auto sa = specificity(a.predicate);
        const auto sb = specificity(b.predicate);
        if (sa != sb) return sa < sb;
        return a.predicate < b.predicate;
    });
}

} // namespace mcpp::manifest::cfg
