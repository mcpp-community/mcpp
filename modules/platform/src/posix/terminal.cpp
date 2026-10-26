// mcpp.platform.terminal on Linux and macOS: the terminal device both
// streams reach, keys read from a terminal without a line discipline, and
// standard output diverted at its descriptor.
module;
#include <cerrno>
#include <cstdio>
#include <fcntl.h>      // fcntl, F_DUPFD_CLOEXEC
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

module mcpp.platform.terminal;

import std;
import mcpp.platform.unix.bounded_process;

namespace mcpp::platform::terminal::native {

bool same_terminal() {
    const int out = ::fileno(stdout), err = ::fileno(stderr);
    if (::isatty(out) == 0 || ::isatty(err) == 0) return false;
    struct stat a{}, b{};
    if (::fstat(out, &a) != 0 || ::fstat(err, &b) != 0) return false;
    return a.st_rdev == b.st_rdev;
}

unsigned console_output_code_page(Stream) { return 0; }

bool write_descriptor(int fd, std::string_view text) {
    std::size_t done = 0;
    while (done < text.size()) {
        const auto n = ::write(fd, text.data() + done, text.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

int divert_stdout() {
    // Not `dup`: its copy survives exec. Descriptor 3 or above, as `dup`
    // would have chosen, so nothing else about the redirection moves.
    const int saved = ::fcntl(1, F_DUPFD_CLOEXEC, 3);
    if (saved >= 0) ::dup2(2, 1);
    return saved;
}

void restore_stdout(int saved) {
    ::dup2(saved, 1);
    ::close(saved);
}

} // namespace mcpp::platform::terminal::native

namespace mcpp::platform::terminal {

KeyInput::KeyInput(Stream display) {
    if (!is_terminal(display)) return;
    if (::isatty(0) == 0 || ::tcgetpgrp(0) != ::getpgrp()) return;
    struct termios mode{};
    if (::tcgetattr(0, &mode) != 0) return;
    mcpp::platform::unixproc::guard_terminal_mode(0);
    struct termios raw = mode;
    raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));   // ISIG stays: Ctrl-C interrupts
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 0;
    if (::tcsetattr(0, TCSANOW, &raw) != 0) {
        mcpp::platform::unixproc::unguard_terminal_mode();
        return;
    }
    active_ = true;
}

KeyInput::~KeyInput() {
    if (!active_) return;
    mcpp::platform::unixproc::unguard_terminal_mode();
}

std::vector<Key> KeyInput::read() {
    std::vector<Key> keys;
    if (!active_) return keys;
    char buf[64];
    while (true) {
        const auto n = ::read(0, buf, sizeof buf);   // VMIN 0, VTIME 0: never blocks
        if (n <= 0) break;
        pending_.append(buf, static_cast<std::size_t>(n));
    }
    return decode_keys(pending_);
}

} // namespace mcpp::platform::terminal
