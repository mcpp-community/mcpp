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
