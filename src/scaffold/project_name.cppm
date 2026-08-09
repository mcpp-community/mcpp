// Portable project/directory identity for `mcpp new`.

export module mcpp.scaffold.project_name;

import std;
import mcpp.platform.project_name;
import mcpp.pm.dependency_selector;

export namespace mcpp::scaffold {

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
    if (mcpp::platform::is_windows_reserved_project_name(spelling)) {
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
