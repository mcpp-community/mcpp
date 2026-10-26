// mcpp.platform.fs_caps on macOS: renamex_np's swap and statfs's type name.
module;
#include <cerrno>
#include <stdio.h>
#include <sys/mount.h>
#include <sys/param.h>

module mcpp.platform.fs_caps;

import std;

namespace mcpp::platform::native {

int exchange(const std::filesystem::path& a, const std::filesystem::path& b) {
    return ::renamex_np(a.c_str(), b.c_str(), RENAME_SWAP) == 0 ? 0 : errno;
}
bool unsupported(int error) { return error == EINVAL || error == ENOTSUP || error == EOPNOTSUPP || error == ENOSYS; }
std::error_category const& category() { return std::generic_category(); }

std::string filesystem_type(const std::filesystem::path& dir) {
    struct statfs st {};
    return ::statfs(dir.c_str(), &st) == 0 ? std::string(st.f_fstypename) : std::string{};
}

} // namespace mcpp::platform::native
