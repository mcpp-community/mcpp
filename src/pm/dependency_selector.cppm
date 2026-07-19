// mcpp.pm.dependency_selector — parse user dependency selectors into
// ordered package-coordinate candidates.

export module mcpp.pm.dependency_selector;

import std;
import mcpp.pm.dep_spec;

export namespace mcpp::pm {

enum class DependencySelectorMode {
    OmittedMcpplibsPriority,
};

struct DependencySelector {
    std::vector<DependencyCoordinate> candidates;
    std::string stableMapKey;
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

inline DependencySelector resolve_dependency_selector(
    std::string_view selector,
    DependencySelectorMode)
{
    DependencySelector out;
    out.stableMapKey = std::string(selector);

    auto segments = split_dependency_selector(selector);
    if (segments.empty()) return out;

    if (segments.size() == 1) {
        out.candidates.push_back(DependencyCoordinate{
            .namespace_ = std::string(kDefaultNamespace),
            .shortName = segments.front(),
        });
        out.candidates.push_back(DependencyCoordinate{
            .namespace_ = {},
            .shortName = segments.front(),
        });
        return out;
    }

    const auto shortName = segments.back();
    const auto nsWithoutShort = join_dependency_segments(
        segments, 0, segments.size() - 1);

    if (segments.front() == kDefaultNamespace) {
        out.candidates.push_back(DependencyCoordinate{
            .namespace_ = nsWithoutShort,
            .shortName = shortName,
        });
        return out;
    }

    out.candidates.push_back(DependencyCoordinate{
        .namespace_ = std::format("{}.{}", kDefaultNamespace, nsWithoutShort),
        .shortName = shortName,
    });
    out.candidates.push_back(DependencyCoordinate{
        .namespace_ = nsWithoutShort,
        .shortName = shortName,
    });
    return out;
}

} // namespace mcpp::pm
