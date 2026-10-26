// mcpp.platform.fs_caps on Linux and macOS: a directory's owner.
module;
#include <sys/stat.h>
#include <unistd.h>

module mcpp.platform.fs_caps;

import std;

namespace mcpp::platform::native {

std::string owner_problem(const std::filesystem::path& dir) {
    struct stat st {};
    if (::stat(dir.c_str(), &st) != 0) return "it cannot be read";
    if (st.st_uid == ::getuid()) return {};
    if (st.st_uid != 0) return "it belongs to another user";
    if (::access(dir.c_str(), W_OK) == 0) return "it belongs to root and this user can write it";
    return {};
}

} // namespace mcpp::platform::native
