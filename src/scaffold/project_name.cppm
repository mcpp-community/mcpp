// Portable project/directory identity for `mcpp new`.

export module mcpp.scaffold.project_name;

import std;
import mcpp.pm.dependency_selector;

export namespace mcpp::scaffold {

// A name Windows reserves for a device, whatever the host: a project created
// on Linux under it cannot be checked out on Windows.
bool is_windows_reserved_project_name(std::string_view name) {
    // Windows reserves the device basename even when an extension is present
    // (`CON.txt`, `LPT1.log`). Comparison is ASCII case-insensitive.
    auto dot = name.find('.');
    auto base = name.substr(0, dot);
    std::string upper;
    upper.reserve(base.size());
    for (unsigned char ch : base) {
        upper.push_back(ch >= 'a' && ch <= 'z'
            ? static_cast<char>(ch - 'a' + 'A')
            : static_cast<char>(ch));
    }
    if (upper == "CON" || upper == "PRN" || upper == "AUX"
        || upper == "NUL") {
        return true;
    }
    if (upper.size() == 4
        && (upper.starts_with("COM") || upper.starts_with("LPT"))
        && upper[3] >= '1' && upper[3] <= '9') {
        return true;
    }
    return false;
}


struct PortableProjectName {
    std::string directoryName;
    std::string namespace_;
    std::string name;
    std::string qualifiedName;
};

struct ProjectNameError {
    std::string message;
};

std::expected<PortableProjectName, ProjectNameError>
validate_project_name(std::string_view spelling) {
    auto fail = [&](std::string message)
        -> std::expected<PortableProjectName, ProjectNameError> {
        return std::unexpected(ProjectNameError{std::move(message)});
    };

    if (spelling.empty()) return fail("project name is empty");
    if (spelling == "." || spelling == "..") {
        return fail(std::format(
            "project name '{}' is a path component, not a package name",
            spelling));
    }
    if (spelling.size() > 255) {
        return fail("project name exceeds the portable 255-byte component limit");
    }
    if (spelling.back() == '.' || spelling.back() == ' ') {
        return fail(std::format(
            "project name '{}' may not end in a dot or space", spelling));
    }
    if (spelling.find("PROJECT") != std::string_view::npos) {
        return fail(std::format(
            "project name '{}' contains the reserved legacy scaffold marker "
            "'PROJECT'", spelling));
    }

    for (unsigned char ch : spelling) {
        if (ch < 0x20 || ch == 0x7f) {
            return fail(std::format(
                "project name '{}' contains a control character", spelling));
        }
        // Union of portable path separators and Windows-invalid component
        // characters. Colons are template delimiters too, so accepting one
        // here would make diagnostics platform-dependent.
        if (ch == '/' || ch == '\\' || ch == '<' || ch == '>' || ch == ':'
            || ch == '"' || ch == '|' || ch == '?' || ch == '*') {
            return fail(std::format(
                "project name '{}' contains a non-portable path character",
                spelling));
        }
    }
    if (std::filesystem::path(spelling).is_absolute()) {
        return fail(std::format(
            "project name '{}' must be one relative directory component",
            spelling));
    }
    if (is_windows_reserved_project_name(spelling)) {
        return fail(std::format(
            "project name '{}' is a reserved Windows device name", spelling));
    }

    auto selector = mcpp::pm::parse_package_selector(spelling);
    if (!selector) return fail(selector.error().message);
    return PortableProjectName{
        .directoryName = std::string(spelling),
        .namespace_ = selector->namespace_.value_or(std::string{}),
        .name = selector->name,
        .qualifiedName = std::string(spelling),
    };
}

} // namespace mcpp::scaffold
