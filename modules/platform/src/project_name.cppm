// Cross-platform filename rules needed by project scaffolding. Kept under the
// platform layer so the scaffold mechanism does not grow Windows-specific
// device-name branches.

export module mcpp.platform.project_name;

import std;

export namespace mcpp::platform {

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

} // namespace mcpp::platform
