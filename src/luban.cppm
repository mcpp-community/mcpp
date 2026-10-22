// mcpp.luban — what a Luban system says about itself (xlings design
// 2026-10-21 §3.7, J5): its root is the system's declared root, and its store
// is where its xlings put it.
//
// A Luban system's /etc/os-release says ID=luban and its /usr/lib/xlings-abi
// is the runtime xlings bound there; its compiler's sysroot is `/`, which is
// then the system itself, not a tree nothing declared. Its packages live in
// the home its root record names (/etc/xlings/usr: <home>/subos/<name>/root/usr),
// which is /xlings for a system installed on a machine and the path of the
// home that made it for one made on a host -- the edition's [xlings] upstream
// says /xlings for both.
//
// `root` is "/" but for a test.

export module mcpp.luban;

import std;

export namespace mcpp::luban {

bool is_luban_root(const std::filesystem::path& root = "/");

// The home whose store the system reads, from its root record; nullopt when
// `root` is not a Luban system or says nothing readable.
std::optional<std::filesystem::path> system_store(const std::filesystem::path& root = "/");

} // namespace mcpp::luban

namespace mcpp::luban {

bool is_luban_root(const std::filesystem::path& root) {
    std::error_code ec;
    if (!std::filesystem::exists(root / "usr/lib/xlings-abi", ec)) return false;
    std::ifstream in(root / "etc/os-release");
    for (std::string line; std::getline(in, line);) {
        if (!line.starts_with("ID=")) continue;
        auto id = line.substr(3);
        if (id.size() >= 2 && (id.front() == '"' || id.front() == '\'')) id = id.substr(1, id.size() - 2);
        return id == "luban";
    }
    return false;
}

std::optional<std::filesystem::path> system_store(const std::filesystem::path& root) {
    if (!is_luban_root(root)) return std::nullopt;
    std::ifstream in(root / "etc/xlings/usr");
    std::string line;
    if (!std::getline(in, line)) return std::nullopt;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    const std::filesystem::path usr = std::filesystem::path(line).lexically_normal();
    // <home>/subos/<name>/root/usr
    const auto rootDir = usr.parent_path(), instance = rootDir.parent_path(), subos = instance.parent_path();
    if (!usr.is_absolute() || usr.filename() != "usr" || rootDir.filename() != "root" || subos.filename() != "subos")
        return std::nullopt;
    const auto home = subos.parent_path();
    std::error_code ec;
    if (!std::filesystem::is_directory(home, ec)) return std::nullopt;
    return home;
}

} // namespace mcpp::luban
