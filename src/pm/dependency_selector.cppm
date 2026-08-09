// mcpp.pm.dependency_selector — parse user dependency selectors into one
// canonical package coordinate.

export module mcpp.pm.dependency_selector;

import std;
import mcpp.pm.dep_spec;

export namespace mcpp::pm {

struct DependencySelector {
    std::vector<DependencyCoordinate> candidates;
    std::string stableMapKey;
};

struct PackageSelector {
    std::optional<std::string> namespace_;
    std::string                name;
    std::string                spelling;
};

struct SelectorError {
    std::string message;
};

inline std::vector<std::string> split_dependency_selector(std::string_view selector)
{
    std::vector<std::string> segments;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= selector.size(); ++i) {
        if (i == selector.size() || selector[i] == '.') {
            segments.emplace_back(selector.substr(start, i - start));
            start = i + 1;
        }
    }
    return segments;
}

inline std::string join_dependency_segments(const std::vector<std::string>& segments,
                                            std::size_t first,
                                            std::size_t last)
{
    std::string out;
    for (std::size_t i = first; i < last && i < segments.size(); ++i) {
        if (!out.empty()) out += ".";
        out += segments[i];
    }
    return out;
}

inline DependencySelector make_direct_dependency_selector(
    std::string_view ns,
    std::string_view shortName,
    std::string_view stableMapKey)
{
    DependencySelector out;
    out.stableMapKey = std::string(stableMapKey);
    out.candidates.push_back(DependencyCoordinate{
        .namespace_ = std::string(ns),
        .shortName = std::string(shortName),
    });
    return out;
}

inline std::expected<PackageSelector, SelectorError>
parse_package_selector(std::string_view spelling)
{
    if (spelling.empty()) {
        return std::unexpected(SelectorError{
            .message = "package selector is empty",
        });
    }

    auto segments = split_dependency_selector(spelling);
    for (auto const& segment : segments) {
        if (segment.empty()) {
            return std::unexpected(SelectorError{
                .message = std::format(
                    "invalid package selector '{}': empty dotted segment",
                    spelling),
            });
        }
        for (unsigned char ch : segment) {
            const bool bareKeyChar = (ch >= 'a' && ch <= 'z')
                || (ch >= 'A' && ch <= 'Z')
                || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
            if (!bareKeyChar) {
                return std::unexpected(SelectorError{
                    .message = std::format(
                        "invalid package selector '{}': segment '{}' may only "
                        "contain ASCII letters, digits, '-' and '_'",
                        spelling, segment),
                });
            }
        }
    }

    PackageSelector out{
        .name = segments.back(),
        .spelling = std::string(spelling),
    };
    if (segments.size() > 1) {
        out.namespace_ = join_dependency_segments(
            segments, 0, segments.size() - 1);
    }
    return out;
}

inline DependencyCoordinate normalize_package_selector(
    const PackageSelector& selector,
    std::string_view defaultNamespace = kDefaultNamespace)
{
    return DependencyCoordinate{
        .namespace_ = selector.namespace_.value_or(
            std::string(defaultNamespace)),
        .shortName = selector.name,
    };
}

inline std::string
format_package_selector(const DependencyCoordinate& coordinate)
{
    if (coordinate.namespace_.empty()
        || coordinate.namespace_ == kDefaultNamespace) {
        return coordinate.shortName;
    }
    return std::format("{}.{}", coordinate.namespace_,
                       coordinate.shortName);
}

// During the one-release exact-selector migration, this is the coordinate an
// older mcpp would have tried first for a compact dotted selector. It is
// diagnostic/lock-compatibility data only; normal resolution never appends it
// to the exact candidate list.
inline std::optional<DependencyCoordinate>
legacy_prefixed_coordinate(const DependencyCoordinate& exact)
{
    if (exact.namespace_.empty()
        || exact.namespace_ == kDefaultNamespace
        || exact.namespace_.starts_with(
            std::string(kDefaultNamespace) + ".")) {
        return std::nullopt;
    }
    return DependencyCoordinate{
        .namespace_ = std::format("{}.{}", kDefaultNamespace,
                                  exact.namespace_),
        .shortName = exact.shortName,
    };
}

// #243 Cargo dep/feat forwarding. Split a `[features]` implied-feature token:
// a token containing '/' means "when this feature is active, request
// <depFeature> from dependency <depKey>" (Cargo `[features] F = ["dep/feat"]`).
// <depKey> is returned verbatim so it lands in the same keyspace as the
// `dependencies` / `featureDeps` maps (both keyed by the raw selector string,
// == stableMapKey). Split on the FIRST '/' (feature names contain no '/').
// A token with no '/', or with an empty dep/feature half, is a plain local
// implied feature → nullopt.
inline std::optional<std::pair<std::string, std::string>>
split_feature_forward_token(std::string_view token)
{
    auto slash = token.find('/');
    if (slash == std::string_view::npos) return std::nullopt;
    auto depKey  = token.substr(0, slash);
    auto depFeat = token.substr(slash + 1);
    if (depKey.empty() || depFeat.empty()) return std::nullopt;
    return std::pair{std::string(depKey), std::string(depFeat)};
}

inline DependencySelector resolve_dependency_selector(std::string_view selector)
{
    DependencySelector out;
    out.stableMapKey = std::string(selector);

    auto parsed = parse_package_selector(selector);
    if (!parsed) return out;
    out.candidates.push_back(normalize_package_selector(*parsed));
    return out;
}

} // namespace mcpp::pm
