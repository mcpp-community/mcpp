// The lock every release of mcpp takes on Linux and macOS: flock(2).
module;
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

module mcpp.home.files;

import std;

namespace mcpp::home::native {

std::intptr_t lock_file(const std::filesystem::path& file) {
    const int fd = ::open(file.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

void unlock_file(std::intptr_t handle) {
    ::flock(static_cast<int>(handle), LOCK_UN);
    ::close(static_cast<int>(handle));
}

}  // namespace mcpp::home::native
