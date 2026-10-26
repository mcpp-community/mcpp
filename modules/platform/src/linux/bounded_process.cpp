// mcpp.platform.unix.bounded_process on Linux: both ends of a pipe are
// close-on-exec from the call that makes them.
module;
#include <fcntl.h>
#include <unistd.h>

module mcpp.platform.unix.bounded_process;

namespace mcpp::platform::unixproc {

int make_pipe(int fds[2]) {
    return ::pipe2(fds, O_CLOEXEC);
}

} // namespace mcpp::platform::unixproc
