// mcpp.platform.unix.bounded_process on macOS: no `pipe2', so the flag is set
// by a second call, inside the LaunchSection every start holds.
module;
#include <fcntl.h>
#include <unistd.h>

module mcpp.platform.unix.bounded_process;

namespace mcpp::platform::unixproc {

int make_pipe(int fds[2]) {
    if (::pipe(fds) != 0) return -1;
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return 0;
}

} // namespace mcpp::platform::unixproc
